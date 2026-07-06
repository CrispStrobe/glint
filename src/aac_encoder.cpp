// glint - AAC-LC encoder core (phase 1: long blocks, CBR-average, ADTS)
// MIT License - Clean-room implementation
//
// One glint_aac_encode call consumes 1024 samples per channel and emits one
// ADTS frame. The MDCT looks back one block, so the stream is delayed by
// 1024 samples; glint_aac_flush emits one final frame covering the tail.
//
// Rate control is a bit-debt controller: each frame's budget is the running
// target minus bits already spent, clamped to the decoder's per-channel input
// buffer (6144 bits). Long-run average hits the requested bitrate exactly;
// individual ADTS frames vary (the format is variable-length by design —
// there is no MP3-style back-pointer, so no desync hazard).

#include "glint/glint.h"

#include "aac_coder.hpp"
#include "aac_mdct.hpp"
#include "aac_psy.hpp"
#include "aac_tables.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {

using namespace glint::aac;
using namespace glint::aac_tables;

constexpr int kAdtsHeaderBits = 56;   // protection_absent = 1
constexpr int kMaxOutBytes = 4096;    // >= 7 + 2*6144/8 with margin
constexpr int kDecoderBufBits = 6144; // per-channel input buffer cap (ISO)

int samplerate_index(int sr) {
    for (int i = 0; i < kNumSampleRates; i++) {
        if (kSampleRates[i] == sr) return i;
    }
    return -1;
}

// Bandwidth cutoff in Hz for a given per-channel bitrate (phase-1 heuristic,
// to be replaced by the psy model; roughly tracks fdk-aac's LC defaults).
double cutoff_hz(int bits_per_sec_per_ch) {
    if (bits_per_sec_per_ch >= 96000) return 20000.0;
    if (bits_per_sec_per_ch >= 64000) return 16000.0;
    if (bits_per_sec_per_ch >= 48000) return 14000.0;
    if (bits_per_sec_per_ch >= 32000) return 12000.0;
    return bits_per_sec_per_ch * 0.375;
}

}  // namespace

struct glint_aac_context {
    int sample_rate;
    int sr_index;
    int num_channels;
    int bitrate_bps;
    int max_sfb;
    int quality;                    // glint_quality

    double prev[2][kAacFrameLen];   // MDCT lookback (last input block)
    double spec[2][kAacFrameLen];   // coded-domain spectra (M/S bands transformed)
    AacChannelPlan plan[2];
    uint8_t ms_used[kMaxSfb];       // per-sfb M/S flags (stereo)
    int ms_present;                 // 0 = none, 1 = per-band flags, 2 = all
    double mask[2][kMaxSfb];        // per-channel shaping masks (coded domain)

    double bits_per_frame;
    double target_acc;
    double bits_spent;

    double emax_run;                // running max band energy (ATH calibration)

    int frames_in;                  // input blocks consumed
    int flushed;

    uint8_t out[kMaxOutBytes];
};

namespace {

// NMR-driven noise shaping, ported from the MP3 nmr_outer_loop with the same
// hard-won constants: shape to 0.125x the mask (~9 dB below), amplify every
// band within 6 dB of the worst offender, accept only iterates that improve
// the summed noise/mask objective under a 1.25x total-noise guard, and run
// the gain re-search under shape_bits = unshaped spend + HALF the leftover
// budget (full-budget shaping consumed the underspend the rate controller
// needs to see — the MP3 stereo-best 5 dB SNR lesson).
constexpr double kShapeTarget = 0.125;
// Backstop only. The real safety is the per-band ceiling (no band may end
// above max(target, its initial ratio)): bidirectional shaping is SUPPOSED
// to raise raw noise in over-coded loud bands, so MP3's tight total-noise
// guard (1.25, tuned for a unidirectional loop) rejected exactly the
// redistribution we want — measured: it froze shaping at ~-0.5 dB NMR.
constexpr double kNoiseGuard = 2.5;
// Offsets are bidirectional (+ = finer, - = coarser than the anchor); the
// +-30 bound keeps any two coded bands' scalefactors within the +-60 dpcm
// range of the scalefactor codebook.
constexpr int kMaxSfOffset = 30;

void shape_channel(glint_aac_context* c, int chn, int budget);

// Per-band M/S decision + in-place transform of c->spec, and the coded-domain
// shaping masks for both channels. The decision compares mask-relative coding
// cost: code the band as M/S iff (t+eM)(t+eS) < (t+eL)(t+eR), t = min of the
// two channels' masks (a perceptual-entropy product rule; energy-only rules
// misfire on out-of-phase content). In M/S bands BOTH channels shape against
// t: noise in M or S lands in decoded L and R (l = m+s, r = m-s), so the
// side channel must never be shaped against a mask the quieter original
// channel does not have — the AAC form of the MP3 side-channel lesson.
void decide_ms(glint_aac_context* c) {
    const uint16_t* swb = kSwbOffsetLong[c->sr_index];
    const int nb = c->max_sfb;

    double maskL[kMaxSfb], maskR[kMaxSfb];
    double e0 = aac_compute_masks(c->spec[0], c->sr_index, nb, c->emax_run, maskL);
    double e1 = aac_compute_masks(c->spec[1], c->sr_index, nb, c->emax_run, maskR);
    if (e0 > c->emax_run) c->emax_run = e0;
    if (e1 > c->emax_run) c->emax_run = e1;

    int n_ms = 0;
    for (int b = 0; b < nb; b++) {
        double eL = 0, eR = 0, eM = 0, eS = 0;
        for (int i = swb[b]; i < swb[b + 1]; i++) {
            double l = c->spec[0][i], r = c->spec[1][i];
            double m = 0.5 * (l + r), s = 0.5 * (l - r);
            eL += l * l;
            eR += r * r;
            eM += m * m;
            eS += s * s;
        }
        double t = maskL[b] < maskR[b] ? maskL[b] : maskR[b];
        if ((t + eM) * (t + eS) < (t + eL) * (t + eR)) {
            c->ms_used[b] = 1;
            n_ms++;
            for (int i = swb[b]; i < swb[b + 1]; i++) {
                double l = c->spec[0][i], r = c->spec[1][i];
                c->spec[0][i] = 0.5 * (l + r);
                c->spec[1][i] = 0.5 * (l - r);
            }
            c->mask[0][b] = t;
            c->mask[1][b] = t;
        } else {
            c->ms_used[b] = 0;
            c->mask[0][b] = maskL[b];
            c->mask[1][b] = maskR[b];
        }
    }
    c->ms_present = (n_ms == 0) ? 0 : (n_ms == nb) ? 2 : 1;
}

}  // namespace

extern "C" {

glint_aac_t glint_aac_create(const struct glint_aac_config* cfg) {
    if (!cfg) return nullptr;
    int sri = samplerate_index(cfg->sample_rate);
    if (sri < 0) return nullptr;
    if (cfg->num_channels < 1 || cfg->num_channels > 2) return nullptr;
    if (cfg->bitrate < 8 || cfg->bitrate > 800) return nullptr;

    glint_aac_context* c = new (std::nothrow) glint_aac_context();
    if (!c) return nullptr;
    std::memset(c, 0, sizeof(*c));
    c->sample_rate = cfg->sample_rate;
    c->sr_index = sri;
    c->num_channels = cfg->num_channels;
    c->bitrate_bps = cfg->bitrate * 1000;
    c->quality = (cfg->quality >= GLINT_QUALITY_BEST) ? GLINT_QUALITY_BEST
                 : (cfg->quality == GLINT_QUALITY_NORMAL) ? GLINT_QUALITY_NORMAL
                                                          : GLINT_QUALITY_SPEED;
    c->bits_per_frame = static_cast<double>(c->bitrate_bps) * kAacFrameLen / c->sample_rate;

    // max_sfb from the bandwidth cutoff: first band whose start line reaches
    // the cutoff ends the coded region.
    double cut = cutoff_hz(c->bitrate_bps / c->num_channels);
    double hz_per_line = 0.5 * c->sample_rate / kAacFrameLen;
    int cut_line = static_cast<int>(cut / hz_per_line);
    const uint16_t* swb = kSwbOffsetLong[sri];
    int nswb = kNumSwbLong[sri];
    int msfb = nswb;
    for (int b = 1; b <= nswb; b++) {
        if (swb[b] >= cut_line) {
            msfb = b;
            break;
        }
    }
    if (msfb < 4) msfb = 4;
    c->max_sfb = msfb;
    return c;
}

int glint_aac_samples_per_frame(glint_aac_t c) {
    (void)c;
    return kAacFrameLen;
}

static const uint8_t* encode_block(glint_aac_context* c, int* out_size) {
    const int ch = c->num_channels;

    // ---- fit both channels under this frame's budget ----
    double avail = c->target_acc - c->bits_spent;
    double cap = static_cast<double>(kDecoderBufBits) * ch;
    if (avail > cap) avail = cap;
    double floor_bits = 0.35 * c->bits_per_frame;
    if (avail < floor_bits) avail = floor_bits;

    int fixed_overhead = kAdtsHeaderBits + 3 /*END*/ + 7 /*worst-case align*/;
    if (ch == 2) {
        fixed_overhead += 3 + 4 + 1 + 11 + 2;  // CPE id, tag, common_window, ics_info, ms_mask
    } else {
        fixed_overhead += 3 + 4 + 11;          // SCE id, tag, ics_info
    }
    int spend = static_cast<int>(avail) - fixed_overhead;
    if (spend < 64) spend = 64;

    const bool shape = c->quality >= GLINT_QUALITY_NORMAL;
    if (ch == 2) {
        decide_ms(c);
        if (c->ms_present == 1) fixed_overhead += c->max_sfb;  // ms_used bits
        spend = static_cast<int>(avail) - fixed_overhead;
        if (spend < 64) spend = 64;

        double e0 = 0, e1 = 0;
        for (int i = 0; i < kAacFrameLen; i++) {
            e0 += c->spec[0][i] * c->spec[0][i];
            e1 += c->spec[1][i] * c->spec[1][i];
        }
        double share = (e0 + e1 > 0) ? e0 / (e0 + e1) : 0.5;
        if (share < 0.3) share = 0.3;
        if (share > 0.7) share = 0.7;
        int budget0 = static_cast<int>(spend * share);
        aac_fit_channel(c->spec[0], c->sr_index, c->max_sfb, budget0, nullptr, -1, &c->plan[0]);
        if (shape) shape_channel(c, 0, budget0);
        int budget1 = spend - c->plan[0].ics_bits;  // leftover flows to ch 1
        aac_fit_channel(c->spec[1], c->sr_index, c->max_sfb, budget1, nullptr, -1, &c->plan[1]);
        if (shape) shape_channel(c, 1, budget1);
    } else {
        if (shape) {
            double e = aac_compute_masks(c->spec[0], c->sr_index, c->max_sfb,
                                         c->emax_run, c->mask[0]);
            if (e > c->emax_run) c->emax_run = e;
        }
        aac_fit_channel(c->spec[0], c->sr_index, c->max_sfb, spend, nullptr, -1, &c->plan[0]);
        if (shape) shape_channel(c, 0, spend);
    }

    // ---- emit raw_data_block into out+7, then prepend the ADTS header ----
    AacBitWriter bw(c->out + 7, kMaxOutBytes - 7);
    if (ch == 2) {
        bw.put(1, 3);                       // id_syn_ele CPE
        bw.put(0, 4);                       // element_instance_tag
        bw.put(1, 1);                       // common_window
        aac_write_ics_info(bw, c->max_sfb);
        bw.put(c->ms_present, 2);           // ms_mask_present
        if (c->ms_present == 1) {
            for (int b = 0; b < c->max_sfb; b++) bw.put(c->ms_used[b], 1);
        }
        aac_write_ics_body(bw, c->plan[0], c->sr_index, false);
        aac_write_ics_body(bw, c->plan[1], c->sr_index, false);
    } else {
        bw.put(0, 3);                       // id_syn_ele SCE
        bw.put(0, 4);                       // element_instance_tag
        aac_write_ics_body(bw, c->plan[0], c->sr_index, true);
    }
    bw.put(7, 3);                           // id_syn_ele END
    bw.byte_align();

    int payload = bw.bytes();
    int frame_len = payload + 7;

    // ADTS header (MPEG-4, AAC-LC, protection absent)
    uint8_t* h = c->out;
    int profile = 1;  // AAC-LC = audio object type 2, coded as 2-1
    h[0] = 0xFF;
    h[1] = 0xF1;      // sync low, ID=0 (MPEG-4), layer 00, protection_absent 1
    h[2] = static_cast<uint8_t>((profile << 6) | (c->sr_index << 2) | 0 |
                                ((ch >> 2) & 1));
    h[3] = static_cast<uint8_t>(((ch & 3) << 6) | ((frame_len >> 11) & 3));
    h[4] = static_cast<uint8_t>((frame_len >> 3) & 0xFF);
    h[5] = static_cast<uint8_t>(((frame_len & 7) << 5) | 0x1F);  // fullness hi
    h[6] = 0xFC;      // fullness lo (0x7FF), 0 raw blocks

    c->target_acc += c->bits_per_frame;
    c->bits_spent += 8.0 * frame_len;

    *out_size = frame_len;
    return c->out;
}

const uint8_t* glint_aac_encode(glint_aac_t c, const int16_t** channel_data, int* out_size) {
    if (!c || !channel_data || !out_size || c->flushed) {
        if (out_size) *out_size = 0;
        return nullptr;
    }
    for (int chn = 0; chn < c->num_channels; chn++) {
        double cur[kAacFrameLen];
        for (int i = 0; i < kAacFrameLen; i++) {
            cur[i] = static_cast<double>(channel_data[chn][i]);
        }
        aac_mdct_long(c->prev[chn], cur, c->spec[chn]);
        std::memcpy(c->prev[chn], cur, sizeof(cur));
    }
    c->frames_in++;
    return encode_block(c, out_size);
}

const uint8_t* glint_aac_encode_float(glint_aac_t c, const float** channel_data, int* out_size) {
    if (!c || !channel_data || !out_size || c->flushed) {
        if (out_size) *out_size = 0;
        return nullptr;
    }
    for (int chn = 0; chn < c->num_channels; chn++) {
        double cur[kAacFrameLen];
        for (int i = 0; i < kAacFrameLen; i++) {
            cur[i] = static_cast<double>(channel_data[chn][i]) * 32768.0;
        }
        aac_mdct_long(c->prev[chn], cur, c->spec[chn]);
        std::memcpy(c->prev[chn], cur, sizeof(cur));
    }
    c->frames_in++;
    return encode_block(c, out_size);
}

const uint8_t* glint_aac_flush(glint_aac_t c, int* out_size) {
    if (!c || !out_size || c->flushed || c->frames_in == 0) {
        if (out_size) *out_size = 0;
        return nullptr;
    }
    c->flushed = 1;
    double zeros[kAacFrameLen];
    std::memset(zeros, 0, sizeof(zeros));
    for (int chn = 0; chn < c->num_channels; chn++) {
        aac_mdct_long(c->prev[chn], zeros, c->spec[chn]);
    }
    return encode_block(c, out_size);
}

void glint_aac_destroy(glint_aac_t c) {
    delete c;
}

}  // extern "C"

namespace {

void shape_channel(glint_aac_context* c, int chn, int budget) {
    using namespace glint::aac;
    AacChannelPlan& plan = c->plan[chn];
    const double* spec = c->spec[chn];
    const int nb = plan.max_sfb;

    const double* mask = c->mask[chn];  // coded-domain masks from encode_block
    double noise[kMaxSfb];
    aac_band_noise(plan, spec, c->sr_index, noise);

    double r0[kMaxSfb];
    double j_best = 0.0, total0 = 0.0, worst = 0.0;
    for (int b = 0; b < nb; b++) {
        total0 += noise[b];
        r0[b] = (mask[b] > 0.0) ? noise[b] / mask[b] : 0.0;
        j_best += r0[b];
        if (r0[b] > worst) worst = r0[b];
    }
    if (worst <= kShapeTarget) return;

    int shape_bits = plan.ics_bits + (budget - plan.ics_bits) / 2;
    if (shape_bits > budget) shape_bits = budget;

    int off[kMaxSfb] = {0};
    bool was_amped[kMaxSfb] = {false};
    AacChannelPlan best = plan;
    AacChannelPlan cur = plan;
    double cur_noise[kMaxSfb];
    std::memcpy(cur_noise, noise, sizeof(double) * nb);

    const int max_iters = (c->quality >= GLINT_QUALITY_BEST) ? 40 : 16;
    int stall = 0;
    for (int iter = 0; iter < max_iters; iter++) {
        double w = 0.0;
        for (int b = 0; b < nb; b++) {
            if (mask[b] > 0.0) {
                double r = cur_noise[b] / mask[b];
                if (r > w) w = r;
            }
        }
        if (w <= kShapeTarget) break;
        // Amplify only the bands close to the worst offender. MP3 uses 0.25
        // (6 dB) here, but its 128k reservoir gives shaping slack; AAC CBR at
        // 128k has none, so a wide amplification front forces the gain anchor
        // up 2+ steps (+3 dB total noise) per round and the noise guard
        // rejects everything. Narrower rounds walk the same path in smaller,
        // acceptable steps.
        double thresh = kShapeTarget > w * 0.5 ? kShapeTarget : w * 0.5;

        bool changed = false;
        for (int b = 0; b < nb; b++) {
            if (mask[b] <= 0.0) continue;
            double r = cur_noise[b] / mask[b];
            // Tier A: any band over the MASK is audible and must be fixed
            // every round — otherwise a gain-anchor rise leaves near-mask
            // bands newly audible and the per-band ceiling vetoes the whole
            // iterate (measured as a total freeze at 128k).
            // Tier B: below the mask, only the near-worst bands move, so the
            // amplification front stays narrow.
            if ((r > 1.0 || r >= thresh) && off[b] < kMaxSfOffset) {
                off[b] += 2;  // 2 sf steps = ~3 dB finer, MP3's amplification grain
                was_amped[b] = true;
                changed = true;
            } else if (r < kShapeTarget * 0.25 && off[b] > -kMaxSfOffset &&
                       !was_amped[b]) {
                // Over-coded band (>=6 dB below the shaping target): donate
                // bits by coarsening, so the gain anchor need not rise. MP3's
                // loop could only amplify; AAC scalefactors go both ways.
                // Hysteresis: never coarsen a band the loop had to rescue.
                off[b] -= 2;
                changed = true;
            }
        }
        if (!changed) break;

        AacChannelPlan cand;
        aac_fit_channel(spec, c->sr_index, nb, shape_bits, off, cur.fit_gain, &cand);
        double cand_noise[kMaxSfb];
        aac_band_noise(cand, spec, c->sr_index, cand_noise);
        double j = 0.0, total = 0.0;
        bool band_ok = true;
        for (int b = 0; b < nb; b++) {
            total += cand_noise[b];
            if (mask[b] > 0.0) {
                double r = cand_noise[b] / mask[b];
                j += r;
                // No band may become audible that wasn't: below the mask
                // (r < 1) noise placement is free — that freedom IS the
                // shaping; a ceiling at the target instead of the mask
                // measured as a near-total freeze at 128k.
                double ceiling = 1.0 > r0[b] ? 1.0 : r0[b];
                if (r > ceiling * 1.05) band_ok = false;
            }
        }
        cur = cand;
        std::memcpy(cur_noise, cand_noise, sizeof(double) * nb);
        if (j < j_best && band_ok && total <= total0 * kNoiseGuard) {
            j_best = j;
            best = cand;
            stall = 0;
        } else if (++stall >= 5) {
            break;
        }
        if (getenv("GLINT_AAC_DEBUG")) {
            fprintf(stderr, "  it%d: j=%.3g best=%.3g total/0=%.2f g=%d bits=%d/%d w=%.2f\n",
                    iter, j, j_best, total / total0, cand.fit_gain,
                    cand.ics_bits, shape_bits, w);
        }
    }
    plan = best;
}

}  // namespace
