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
#include "aac_tables.hpp"

#include <cmath>
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

    double prev[2][kAacFrameLen];   // MDCT lookback (last input block)
    double spec[2][kAacFrameLen];
    AacChannelPlan plan[2];

    double bits_per_frame;
    double target_acc;
    double bits_spent;

    int frames_in;                  // input blocks consumed
    int flushed;

    uint8_t out[kMaxOutBytes];
};

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

    if (ch == 2) {
        double e0 = 0, e1 = 0;
        for (int i = 0; i < kAacFrameLen; i++) {
            e0 += c->spec[0][i] * c->spec[0][i];
            e1 += c->spec[1][i] * c->spec[1][i];
        }
        double share = (e0 + e1 > 0) ? e0 / (e0 + e1) : 0.5;
        if (share < 0.3) share = 0.3;
        if (share > 0.7) share = 0.7;
        int budget0 = static_cast<int>(spend * share);
        aac_fit_channel(c->spec[0], c->sr_index, c->max_sfb, budget0, &c->plan[0]);
        int budget1 = spend - c->plan[0].ics_bits;  // leftover flows to ch 1
        aac_fit_channel(c->spec[1], c->sr_index, c->max_sfb, budget1, &c->plan[1]);
    } else {
        aac_fit_channel(c->spec[0], c->sr_index, c->max_sfb, spend, &c->plan[0]);
    }

    // ---- emit raw_data_block into out+7, then prepend the ADTS header ----
    AacBitWriter bw(c->out + 7, kMaxOutBytes - 7);
    if (ch == 2) {
        bw.put(1, 3);                       // id_syn_ele CPE
        bw.put(0, 4);                       // element_instance_tag
        bw.put(1, 1);                       // common_window
        aac_write_ics_info(bw, c->max_sfb);
        bw.put(0, 2);                       // ms_mask_present = 0
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
