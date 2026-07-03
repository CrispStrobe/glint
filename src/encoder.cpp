// glint - Encoder orchestrator implementation
// MIT License - Clean-room implementation

#include "encoder.hpp"
#include "fixedpoint.hpp"
#include "mdct.hpp"
#include "simd.hpp"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <algorithm>

using namespace glint;

#if !defined(GLINT_FIXED_POINT) || defined(GLINT_BOTH_PATHS)
// Reorder short-block MDCT coefficients from [subband][window][freq] to
// the order expected by the decoder: grouped by scalefactor band, then
// window interleaved.  ISO 11172-3 B.8 short block reorder.
// Input:  mdct_short[32][3][6] = 576 coefficients
// Output: flat[576] reordered for Huffman encoding
static void reorder_short_blocks(const double mdct_short[32][3][6],
                                 double flat[576], int sr_index) {
    const int* sfb = tables::get_sfb_short_by_unified(sr_index);
    // Zero the output
    std::memset(flat, 0, 576 * sizeof(double));

    // Short block SFB boundaries are per-window (max 192 lines per window).
    // Total output is 3 * 192 = 576.
    // The reorder maps: for each SFB band, for each window, for each freq line
    // in that band, write sequentially.
    int out_idx = 0;
    for (int sfbi = 0; sfbi < 13; sfbi++) {
        int width = sfb[sfbi + 1] - sfb[sfbi];
        for (int win = 0; win < 3; win++) {
            for (int j = 0; j < width; j++) {
                int freq = sfb[sfbi] + j;
                // freq is the index within a single window's 192 lines
                // Map to [subband][freq_within_subband]:
                // Each subband contributes 6 freq lines, so:
                int sb = freq / 6;
                int k = freq % 6;
                if (sb < 32) {
                    flat[out_idx] = mdct_short[sb][win][k];
                }
                out_idx++;
            }
        }
    }
}

// Per-frame block-type scheduler. Decides one window type per granule of
// the frame being emitted, shared by both channels (required for M/S
// coding), keeping the ISO window chain valid:
//   ... long, START(1), SHORT(2)..., STOP(3), long ...
// gr_energy holds num_gr+1 entries: the frame's granules plus the held
// lookahead granule. Thanks to the one-granule encoder delay, a transient in
// ANY granule gets a proper START on the granule before it (the lookahead
// entry schedules a START on this frame's last granule when the held granule
// is transient; the carry then makes it SHORT next frame).
// Attack-decay breadth: granules to keep SHORT after each transient, so the
// burst decay stays in short windows too (a castanet burst decays over ~2
// granules). Measured on castanets-128k: p95 NMR 9.5 -> 3.1 and audible
// band-frames 10.8% -> 8.5% (2 extra granules; 1 got only 5.3/9.4). SNR
// drops on the extended granules by design — judge transients by NMR.
// Speech/music are untouched (no spurious extensions measured).
static constexpr int kShortAttackExtend = 2;

static void schedule_block_types(glint_context* enc,
                                 const double gr_energy[3], int num_gr,
                                 int types[2]) {
    bool want[3] = { false, false, false };
    for (int g = 0; g < num_gr + 1; g++) {
        if (enc->sched_energy_valid && enc->sched_prev_energy > 0.0 &&
            gr_energy[g] > 8.0 * enc->sched_prev_energy)
            want[g] = true;   // >9 dB energy jump = transient
        enc->sched_prev_energy = gr_energy[g];
        enc->sched_energy_valid = true;
    }
    // Attack-decay breadth: keep the next kShortAttackExtend granules SHORT
    // after a transient (the run carries across frames via sched_short_run).
    for (int g = 0; g < num_gr; g++) {
        if (want[g]) enc->sched_short_run = kShortAttackExtend;
        else if (enc->sched_short_run > 0) {
            want[g] = true;
            enc->sched_short_run--;
        }
    }
    // Next call re-evaluates the lookahead granule as its first work granule
    // against the energy of the granule before it.
    enc->sched_prev_energy = gr_energy[num_gr - 1];

    for (int g = 0; g < num_gr; g++) types[g] = 0;

    // Carry-in from the previous frame's window chain.
    if (enc->next_block_carry == 2) types[0] = 2;
    else if (enc->next_block_carry == 3) types[0] = want[0] ? 2 : 3;

    for (int g = 0; g < num_gr; g++) {
        if (types[g] != 0 || !want[g]) continue;
        int prev = (g > 0) ? types[g - 1] : -1;  // -1: previous frame (long)
        if (prev == 1 || prev == 2) types[g] = 2;
        else if (prev == 0) { types[g - 1] = 1; types[g] = 2; }
        else types[g] = 1;  // start now, short next (right after a STOP etc.)
    }
    // Lookahead: the held granule is transient — give it a START now.
    if (want[num_gr] && types[num_gr - 1] == 0) types[num_gr - 1] = 1;

    // Enforce the forward chain inside the frame.
    for (int g = 1; g < num_gr; g++) {
        if (types[g - 1] == 1 && types[g] != 2) types[g] = 2;
        else if (types[g - 1] == 2 && types[g] == 0) types[g] = 3;
    }

    int last = types[num_gr - 1];
    enc->next_block_carry = (last == 1) ? 2 : (last == 2) ? 3 : 0;

    // TEMP DIAG: force every granule short
    if (getenv("GLINT_FORCE_SHORT"))
        for (int g = 0; g < num_gr; g++) types[g] = 2;
}

// Short blocks: enabled together with the transition-window scheduler above.
static constexpr bool kShortBlocksEnabled = true;

#endif // double-precision path helpers

// Encoder-side lowpass at the sfb21 boundary (~15.8 kHz at 44.1k, ~10 kHz
// at 22.05k): zero the region before quantization. The sfb21 region carries
// NO scalefactor, so the psy loop cannot shape its noise — left alone the
// quantizer sprays noise into it (measured -1 dB band-SNR above 16k on
// castanets, i.e. more noise than signal, and audible hiss into EMPTY HF
// bands on tonal music). Zeroing it improves mean NMR on every test clip
// at every rate (castanets-128k 15.0 -> 8.7, quartet-256k -10.1 -> -15.1,
// VBR V0 speech +0.27 dB SNR) and matches LAME's measured 128k spectrum
// almost exactly (rolloff 15.1k vs 15.05k). Cost: content above the
// boundary is dropped (LSD rises on clips that had any); at 256k LAME
// keeps up to ~17.7k instead — revisit if a shapeable sfb21 alternative
// (e.g. gain-region trades) ever lands. Applied in ALL modes and paths so
// double==fixed metrics hold. Attack-only zeroing (window-switching
// granules) kept the p95 win but lost the mean win — most spray comes from
// LONG granules between transients.
static void apply_sfb21_lowpass(double* mdct_flat, int sr_index,
                                bool short_block) {
    int start = short_block
        ? tables::get_sfb_short_by_unified(sr_index)[12] * 3
        : tables::get_sfb_long_by_unified(sr_index)[21];
    for (int i = start; i < 576; i++) mdct_flat[i] = 0.0;
}

int glint_check_config(int sample_rate, int bitrate) {
    if (tables::samplerate_to_index(sample_rate) < 0) return -1;
    int mpeg_ver = tables::detect_mpeg_version(sample_rate);
    if (mpeg_ver == 1) {
        if (tables::bitrate_to_index(bitrate) == 0) return -1;
    } else {
        if (tables::bitrate_to_index_m2(bitrate) == 0) return -1;
    }
    return 0;
}

void glint_set_threads(int num_threads) {
    quantize_set_threads(num_threads);
}

glint_t glint_create(const glint_config* cfg) {
    if (!cfg) return nullptr;

    // For VBR, quantize under the max-bitrate frame budget regardless of -b
    // (each frame is then emitted at the smallest bitrate index that fits).
    // MPEG-2/2.5 rates top out at 160 kbps — forcing 320 made glint_create
    // reject VBR at low sample rates outright.
    int effective_bitrate = cfg->bitrate;
    if (cfg->vbr == GLINT_VBR_ON) {
        effective_bitrate =
            (tables::detect_mpeg_version(cfg->sample_rate) == 1) ? 320 : 160;
    }

    if (glint_check_config(cfg->sample_rate, effective_bitrate) != 0) return nullptr;
    if (cfg->num_channels < 1 || cfg->num_channels > 2) return nullptr;
    if (cfg->num_channels == 1 && cfg->mode != GLINT_MONO) return nullptr;
    if (cfg->num_channels == 2 && cfg->mode == GLINT_MONO) return nullptr;

    tables::init_tables();
    init_simd(cfg->simd);

    auto* ctx = new (std::nothrow) glint_context{};
    if (!ctx) return nullptr;

    ctx->config = *cfg;
    // Override the stored bitrate to 320 for VBR
    if (cfg->vbr == GLINT_VBR_ON) {
        ctx->config.bitrate = effective_bitrate;
    }
    ctx->mpeg_version = tables::detect_mpeg_version(cfg->sample_rate);
    ctx->sr_index = tables::make_unified_sr_index(
        ctx->mpeg_version, tables::samplerate_to_index(cfg->sample_rate));
    ctx->num_granules = (ctx->mpeg_version == 1) ? 2 : 1;

    // Use the effective bitrate (320 for VBR) — computing these from the
    // caller's cfg->bitrate silently ran VBR on the caller's (default 128k)
    // frame budget despite the documented 320k VBR frame size.
    if (ctx->mpeg_version == 1) {
        ctx->br_index = tables::bitrate_to_index(effective_bitrate);
    } else {
        ctx->br_index = tables::bitrate_to_index_m2(effective_bitrate);
    }

    ctx->num_channels = cfg->num_channels;
    ctx->frame_count = 0;
    ctx->quality_mode = static_cast<int>(cfg->quality);
    ctx->vbr_mode = (cfg->vbr == GLINT_VBR_ON);
    ctx->vbr_quality = cfg->vbr_quality;

    if (ctx->mpeg_version == 1) {
        // MPEG-1: side info is 17 bytes (mono) or 32 bytes (stereo)
        ctx->side_info_bits = (cfg->num_channels == 1) ? 136 : 256;
    } else {
        // MPEG-2/2.5: side info is 9 bytes (mono) or 17 bytes (stereo)
        ctx->side_info_bits = (cfg->num_channels == 1) ? 72 : 136;
    }

    int bitrate_bps = effective_bitrate * 1000;
    if (ctx->mpeg_version == 1) {
        ctx->frame_size = 144 * bitrate_bps / cfg->sample_rate;
    } else {
        ctx->frame_size = 72 * bitrate_bps / cfg->sample_rate;
    }
    ctx->padding_threshold = cfg->sample_rate;
    ctx->padding_remainder = 0;

    int total_frame_bits = ctx->frame_size * 8;
    ctx->mean_bits_per_frame = total_frame_bits - 32 - ctx->side_info_bits;

    // Reservoir capacity = what the main_data_begin field can express:
    // 9 bits (511 bytes) for MPEG-1, 8 bits (255) for MPEG-2/2.5.
    ctx->reservoir.init(ctx->mpeg_version == 1 ? 511 : 255);
    ctx->rc_anchor = 0;
    ctx->rc_gain_ema_x16 = 0;

    // Determine signal path
#ifdef GLINT_BOTH_PATHS
    ctx->use_fixed_point = (cfg->path != GLINT_PATH_DOUBLE);
#elif defined(GLINT_FIXED_POINT)
    ctx->use_fixed_point = true;
#else
    ctx->use_fixed_point = false;
#endif

    ctx->sched_prev_energy = 0.0;
    ctx->sched_energy_valid = false;
    ctx->next_block_carry = 0;
    ctx->sched_short_run = 0;
#if !defined(GLINT_FIXED_POINT) || defined(GLINT_BOTH_PATHS)
    std::memset(ctx->held_sub_d, 0, sizeof(ctx->held_sub_d));
    ctx->have_held = false;
#endif

    ctx->write_cb = nullptr;
    ctx->write_cb_data = nullptr;

    for (int ch = 0; ch < ctx->num_channels; ch++) {
#if !defined(GLINT_FIXED_POINT) || defined(GLINT_BOTH_PATHS)
        ctx->subband[ch].reset();
        ctx->mdct[ch].reset();
#endif
#ifdef GLINT_FIXED_POINT
        ctx->subband_fp[ch].reset();
        ctx->mdct_fp[ch].reset();
#endif
    }

    return ctx;
}

glint_t glint_create_streaming(const glint_config* cfg, glint_write_cb cb, void* ud) {
    glint_t enc = glint_create(cfg);
    if (enc) {
        enc->write_cb = cb;
        enc->write_cb_data = ud;
    }
    return enc;
}

int glint_samples_per_frame(glint_t enc) {
    if (enc && enc->mpeg_version != 1) {
        return 576;  // MPEG-2/2.5: 1 granule = 576 samples
    }
    return 1152;  // MPEG-1: 2 granules = 1152 samples
}

static void write_granule_side_info(BitstreamWriter& bs, const GranuleInfo& gi,
                                    int mpeg_version = 1) {
    bs.write_bits(gi.part2_3_length, 12);
    bs.write_bits(gi.regions.big_values, 9);
    bs.write_bits(gi.global_gain, 8);
    if (mpeg_version == 1) {
        bs.write_bits(gi.scalefac_compress, 4);
    } else {
        // MPEG-2/2.5: scalefac_compress is 9 bits
        bs.write_bits(gi.scalefac_compress, 9);
    }
    bs.write_bits(gi.block_type != 0 ? 1 : 0, 1);  // window_switching_flag
    if (gi.block_type != 0) {
        bs.write_bits(gi.block_type, 2);     // block_type (2 = short)
        bs.write_bits(0, 1);                 // mixed_block_flag
        bs.write_bits(gi.regions.table_select[0], 5);
        bs.write_bits(gi.regions.table_select[1], 5);
        // subblock_gain[0..2] (3 x 3 bits; nonzero only for block_type 2)
        bs.write_bits(gi.subblock_gain[0], 3);
        bs.write_bits(gi.subblock_gain[1], 3);
        bs.write_bits(gi.subblock_gain[2], 3);
    } else {
        bs.write_bits(gi.regions.table_select[0], 5);
        bs.write_bits(gi.regions.table_select[1], 5);
        bs.write_bits(gi.regions.table_select[2], 5);
        bs.write_bits(gi.regions.region0_count, 4);
        bs.write_bits(gi.regions.region1_count, 3);
    }
    if (mpeg_version == 1) {
        bs.write_bits(gi.preflag, 1);
    }
    // No preflag for MPEG-2/2.5
    bs.write_bits(gi.scalefac_scale, 1);
    bs.write_bits(gi.regions.count1table, 1);
}

static int mode_to_mpeg(glint_mode mode) {
    switch (mode) {
    case GLINT_STEREO: return 0;
    case GLINT_JOINT:  return 1;
    case GLINT_DUAL:   return 2;
    case GLINT_MONO:   return 3;
    default:            return 0;
    }
}

// Q31 constant for 1/sqrt(2) used in MS stereo
static constexpr int32_t kInvSqrt2_Q31 = 1518500250; // 0.7071067811865476 * 2^31

// Inter-granule 30/70 bit redistribution (-q best). Disabled: it was a net
// win only while the pow34 curve bug capped quantization accuracy; with the
// curve fixed it starves the quiet granule and costs ~1.9 dB SNR at -q best
// on the speech reference (34.7 -> 32.8). Revisit with the bit reservoir.
static constexpr bool kGranuleRedistribution = false;

// VBR frame fit: smallest header bitrate index whose frame holds this
// frame's actual data (header + side info + main data). VBR granules
// quantize under the 320 kbps budget; each frame is then emitted at the
// smallest size that fits instead of padding every frame to 320 kbps.
// Returns the header index and sets *frame_size_out (padding bit stays 0).
static int vbr_pick_frame_size(const glint_context* enc, int total_main_bits,
                               int* frame_size_out) {
    int needed = 4 + enc->side_info_bits / 8 + (total_main_bits + 7) / 8;
    int slot_mult = (enc->mpeg_version == 1) ? 144 : 72;
    for (int i = 0; i < tables::kNumBitrates; i++) {
        int kbps = (enc->mpeg_version == 1) ? tables::kBitrates[i]
                                            : tables::kBitrates_M2[i];
        int size = slot_mult * kbps * 1000 / enc->config.sample_rate;
        if (size >= needed) {
            *frame_size_out = size;
            return i + 1;
        }
    }
    // Cannot happen (granule budgets cap main data at the max-bitrate frame),
    // but fall back to the max bitrate.
    *frame_size_out = slot_mult *
        ((enc->mpeg_version == 1) ? tables::kBitrates[tables::kNumBitrates - 1]
                                  : tables::kBitrates_M2[tables::kNumBitrates - 1]) *
        1000 / enc->config.sample_rate;
    return tables::kNumBitrates;
}

// Per-granule channel bit-split clamp (share of the granule's total that
// channel 0 — mid, in M/S — may receive). Tuned on the speech reference:
// wider clamps starve the side channel in loud passages and regress global
// SNR in joint mode even though mid-band metrics improve.
static constexpr double kChSplitLo = 0.45;
static constexpr double kChSplitHi = 0.55;

// Shared frame-emission tail for all encode paths: SCFSI, header, side info,
// main data, then either the bit-reservoir stream (CBR: frames are buffered
// and released once their slot is full, so *out_size may be 0 or cover
// several frames) or a direct self-contained frame (VBR). granule_info is
// mutated by the SCFSI adjustment.
static const uint8_t* finish_frame(glint_t enc, GranuleInfo granule_info[2][2],
                                   int num_gr, int nch, int mode_ext,
                                   int this_frame_size, int total_main_bits,
                                   int* out_size) {
    // Compute SCFSI: share scalefactors between granules when identical
    // (MPEG-1 only). Groups: 0=bands 0-5, 1=6-10, 2=11-15, 3=16-20.
    static const int scfsi_band[5] = {0, 6, 11, 16, 21};
    int scfsi[2][4] = {};
    if (num_gr == 2) {
        for (int ch = 0; ch < nch; ch++) {
            // SCFSI not allowed when either granule uses short blocks
            bool has_short = (granule_info[0][ch].block_type != 0) ||
                             (granule_info[1][ch].block_type != 0);
            for (int group = 0; group < 4; group++) {
                bool match = !has_short;
                if (match) {
                    for (int b = scfsi_band[group]; b < scfsi_band[group+1] && b < 21; b++) {
                        if (granule_info[0][ch].scalefac[b] != granule_info[1][ch].scalefac[b]) {
                            match = false;
                            break;
                        }
                    }
                }
                scfsi[ch][group] = match ? 1 : 0;
            }
            // Adjust granule 1's part2_length: omit shared scalefactor bits
            if (granule_info[1][ch].part2_length > 0) {
                static const int slen_table_scfsi[16][2] = {
                    {0,0},{0,1},{0,2},{0,3},{3,0},{1,1},{1,2},{1,3},
                    {2,1},{2,2},{2,3},{3,1},{3,2},{3,3},{4,2},{4,3}
                };
                int slen1 = slen_table_scfsi[granule_info[1][ch].scalefac_compress][0];
                int slen2 = slen_table_scfsi[granule_info[1][ch].scalefac_compress][1];
                int saved = 0;
                if (scfsi[ch][0]) saved += slen1 * 6;
                if (scfsi[ch][1]) saved += slen1 * 5;
                if (scfsi[ch][2]) saved += slen2 * 5;
                if (scfsi[ch][3]) saved += slen2 * 5;
                granule_info[1][ch].part2_length -= saved;
                granule_info[1][ch].part2_3_length -= saved;
                total_main_bits -= saved;
            }
        }
    }

    // Write frame header (the header needs the 2-bit sample rate index)
    int channel_mode = mode_to_mpeg(enc->config.mode);
    int hdr_sr_index = (enc->sr_index < 3) ? enc->sr_index : (enc->sr_index - 3);
    enc->frame_asm.reset();
    int hdr_br_index = enc->br_index;
    int hdr_padding = enc->padding;
    if (enc->vbr_mode) {
        hdr_br_index = vbr_pick_frame_size(enc, total_main_bits,
                                           &this_frame_size);
        hdr_padding = 0;
    }
    enc->frame_asm.write_header(hdr_br_index, hdr_sr_index,
                                 hdr_padding, channel_mode, mode_ext,
                                 enc->mpeg_version);

    // Side info, including this frame's main_data_begin (VBR frames are
    // self-contained: 0).
    BitstreamWriter& si = enc->frame_asm.side_info();
    si.reset();
    int main_data_begin = enc->vbr_mode ? 0 : enc->reservoir.main_data_begin();

    if (enc->mpeg_version == 1) {
        si.write_bits(main_data_begin, 9);
        if (nch == 1) si.write_bits(0, 5); else si.write_bits(0, 3);
        for (int ch = 0; ch < nch; ch++) {
            si.write_bits(scfsi[ch][0], 1);
            si.write_bits(scfsi[ch][1], 1);
            si.write_bits(scfsi[ch][2], 1);
            si.write_bits(scfsi[ch][3], 1);
        }
        for (int gr = 0; gr < 2; gr++)
            for (int ch = 0; ch < nch; ch++)
                write_granule_side_info(si, granule_info[gr][ch], enc->mpeg_version);
    } else {
        si.write_bits(main_data_begin, 8);
        if (nch == 1) si.write_bits(0, 1); else si.write_bits(0, 2);
        for (int ch = 0; ch < nch; ch++)
            write_granule_side_info(si, granule_info[0][ch], enc->mpeg_version);
    }
    si.byte_align();

    // Main data: scalefactors + Huffman for each granule/channel.
    BitstreamWriter& md = enc->frame_asm.main_data();
    md.reset();

    static const int slen_table_m1[16][2] = {
        {0,0},{0,1},{0,2},{0,3},{3,0},{1,1},{1,2},{1,3},
        {2,1},{2,2},{2,3},{3,1},{3,2},{3,3},{4,2},{4,3}
    };

    for (int gr = 0; gr < num_gr; gr++) {
        for (int ch = 0; ch < nch; ch++) {
            const GranuleInfo& gi = granule_info[gr][ch];

            if (enc->mpeg_version != 1) {
                // MPEG-2/2.5: decode 9-bit scalefac_compress to 4 slen values
                int sfc = gi.scalefac_compress;
                int slen[4] = {};
                int nr[4] = {6, 5, 5, 5};
                // ISO 13818-3 mapping (must match what real decoders derive;
                // the encoder only ever emits the sfc < 400 range)
                if (sfc < 400) {
                    slen[0] = (sfc >> 4) / 5;
                    slen[1] = (sfc >> 4) % 5;
                    slen[2] = (sfc & 15) >> 2;
                    slen[3] = sfc & 3;
                } else if (sfc < 500) {
                    int v = sfc - 400;
                    slen[0] = (v >> 2) / 5;
                    slen[1] = (v >> 2) % 5;
                    slen[2] = v & 3;
                    slen[3] = 0;
                    nr[0] = 6; nr[1] = 5; nr[2] = 7; nr[3] = 3;
                } else {
                    int v = sfc - 500;
                    slen[0] = v / 3;
                    slen[1] = v % 3;
                    slen[2] = 0;
                    slen[3] = 0;
                    nr[0] = 11; nr[1] = 10; nr[2] = 0; nr[3] = 0;
                }
                if (gi.block_type == 2) {
                    // LSF short (non-mixed): the slen groups cover 3 bands x
                    // 3 windows = 9 values each, written in [band][window]
                    // wire order (band group = band / 3).
                    for (int b = 0; b < 12; b++)
                        for (int w = 0; w < 3; w++)
                            if (slen[b / 3] > 0)
                                md.write_bits(gi.scalefac_s[b][w], slen[b / 3]);
                } else {
                    int b = 0;
                    for (int g = 0; g < 4; g++) {
                        for (int i = 0; i < nr[g] && b < 21; i++, b++) {
                            if (slen[g] > 0)
                                md.write_bits(gi.scalefac[b], slen[g]);
                        }
                    }
                }
            } else if (gi.block_type == 2) {
                // MPEG-1 short-block scalefactors: 12 short sfbs x 3 windows
                // in wire order [band][window]; slen1 covers bands 0-5,
                // slen2 bands 6-11 (no SCFSI for window-switching granules).
                int slen1 = slen_table_m1[gi.scalefac_compress][0];
                int slen2 = slen_table_m1[gi.scalefac_compress][1];
                for (int b = 0; b < 6; b++)
                    for (int w = 0; w < 3; w++)
                        if (slen1 > 0) md.write_bits(gi.scalefac_s[b][w], slen1);
                for (int b = 6; b < 12; b++)
                    for (int w = 0; w < 3; w++)
                        if (slen2 > 0) md.write_bits(gi.scalefac_s[b][w], slen2);
            } else {
                // MPEG-1 scalefactor encoding
                int slen1 = slen_table_m1[gi.scalefac_compress][0];
                int slen2 = slen_table_m1[gi.scalefac_compress][1];

                if (gr == 0) {
                    for (int b = 0; b < 11; b++)
                        if (slen1 > 0) md.write_bits(gi.scalefac[b], slen1);
                    for (int b = 11; b < 21; b++)
                        if (slen2 > 0) md.write_bits(gi.scalefac[b], slen2);
                } else {
                    // Granule 1: skip groups where scfsi=1
                    for (int b = 0; b < 6; b++)
                        if (slen1 > 0 && !scfsi[ch][0]) md.write_bits(gi.scalefac[b], slen1);
                    for (int b = 6; b < 11; b++)
                        if (slen1 > 0 && !scfsi[ch][1]) md.write_bits(gi.scalefac[b], slen1);
                    for (int b = 11; b < 16; b++)
                        if (slen2 > 0 && !scfsi[ch][2]) md.write_bits(gi.scalefac[b], slen2);
                    for (int b = 16; b < 21; b++)
                        if (slen2 > 0 && !scfsi[ch][3]) md.write_bits(gi.scalefac[b], slen2);
                }
            }

            huffman_encode(gi.ix, gi.regions, enc->sr_index, md);
        }
    }
    md.flush();

    if (enc->vbr_mode) {
        // VBR: each frame is self-contained.
        const uint8_t* frame = enc->frame_asm.assemble(this_frame_size, out_size);
        std::memcpy(enc->output_buf, frame, *out_size);
    } else {
        // CBR: append to the reservoir stream; complete slots are released.
        uint8_t hs[64];
        std::memcpy(hs, enc->frame_asm.header().data(), 4);
        int si_bytes = si.byte_count();
        std::memcpy(hs + 4, si.data(), si_bytes);
        int hs_len = 4 + si_bytes;
        int slot_md = this_frame_size - hs_len;
        *out_size = enc->reservoir.add_frame(hs, hs_len, md.data(),
                                             md.byte_count(), slot_md,
                                             enc->output_buf,
                                             sizeof(enc->output_buf));
    }

    enc->frame_count++;
    if (enc->write_cb && *out_size > 0)
        enc->write_cb(enc->output_buf, *out_size, enc->write_cb_data);
    return enc->output_buf;
}

const uint8_t* glint_encode(glint_t enc, const int16_t** channel_data,
                              int* out_size) {
    if (!enc || !channel_data || !out_size) {
        if (out_size) *out_size = 0;
        return nullptr;
    }

    int nch = enc->num_channels;
    bool use_ms = (enc->config.mode == GLINT_JOINT && nch == 2);

    // Padding
    enc->padding = 0;
    int bitrate_bps = enc->config.bitrate * 1000;
    int frame_slot_mult = (enc->mpeg_version == 1) ? 144 : 72;
    int remainder = (frame_slot_mult * bitrate_bps) % enc->config.sample_rate;
    enc->padding_remainder += remainder;
    if (enc->padding_remainder >= enc->config.sample_rate) {
        enc->padding_remainder -= enc->config.sample_rate;
        enc->padding = 1;
    }

    // VBR frames are emitted unpadded (vbr_pick_frame_size), so the
    // quantization budget must not assume the padding byte — otherwise a
    // full frame can need one byte more than the largest emitted frame.
    int this_frame_size = enc->frame_size + (enc->vbr_mode ? 0 : enc->padding);
    int this_frame_bits = this_frame_size * 8 - 32 - enc->side_info_bits;

    // Bit reservoir rate control (CBR): a frame may spend its own slot plus
    // the reservoir (capped at one extra slot), but a per-frame gain FLOOR —
    // the constant-quality anchor — stops the quantizer from spending finer
    // than the current quality target, so easy frames bank bits instead of
    // gold-plating. The anchor adapts +-1 gain step per frame from reservoir
    // fill (>60% full: aim finer / spend; <40%: aim coarser / save) and is
    // tethered to the EMA of achieved gains so it tracks the content's
    // operating point. This replaces the measured-bad naive policy ("every
    // frame may spend the whole reservoir", which oscillated); see PLAN.md
    // item 5.
    int available_bits = this_frame_bits;
    int gain_floor = 0;
    if (!enc->vbr_mode) {
        int mdb_bits = 8 * enc->reservoir.main_data_begin();
        available_bits += std::min(mdb_bits, this_frame_bits);
        if (enc->rc_anchor > 0) {
            int resv_cap_bits = 8 * (enc->mpeg_version == 1 ? 511 : 255);
            if (mdb_bits * 10 > resv_cap_bits * 6) enc->rc_anchor--;
            else if (mdb_bits * 10 < resv_cap_bits * 4) enc->rc_anchor++;
            int ema = enc->rc_gain_ema_x16 / 16;
            if (enc->rc_anchor < ema - 12) enc->rc_anchor = ema - 12;
            if (enc->rc_anchor > ema + 12) enc->rc_anchor = ema + 12;
            if (enc->rc_anchor < 1) enc->rc_anchor = 1;
            if (enc->rc_anchor > 255) enc->rc_anchor = 255;
            gain_floor = enc->rc_anchor;
        }
    }

    int num_gr = enc->num_granules;
    int bits_per_granule = available_bits / (num_gr * nch);
    if (bits_per_granule < 0) bits_per_granule = 0;

    int num_slots = num_gr * 18;
    GranuleInfo granule_info[2][2];
    int total_main_bits = 0;
    int mode_ext = 0;

#if !defined(GLINT_FIXED_POINT) || defined(GLINT_BOTH_PATHS)
    // Helper lambda: encode one frame through the double-precision path
    auto encode_double = [&]() {
        double sb_new[2][32][36];
        for (int ch = 0; ch < nch; ch++)
            enc->subband[ch].analyze(channel_data[ch], sb_new[ch], num_slots);

        // One-granule encoder delay: the emitted frame is the held granule
        // followed by this call's granules except the last, which becomes the
        // new held (lookahead) granule. This is what lets the scheduler place
        // a START window on the granule BEFORE every transient. glint_flush
        // releases the final held granule.
        double subband_out_d[2][32][36];
        for (int ch = 0; ch < nch; ch++)
            for (int sb = 0; sb < 32; sb++) {
                for (int ts = 0; ts < 18; ts++)
                    subband_out_d[ch][sb][ts] =
                        enc->have_held ? enc->held_sub_d[ch][sb][ts] : 0.0;
                for (int g = 1; g < num_gr; g++)
                    for (int ts = 0; ts < 18; ts++)
                        subband_out_d[ch][sb][g * 18 + ts] =
                            sb_new[ch][sb][(g - 1) * 18 + ts];
                for (int ts = 0; ts < 18; ts++)
                    enc->held_sub_d[ch][sb][ts] =
                        sb_new[ch][sb][(num_gr - 1) * 18 + ts];
            }
        enc->have_held = true;

        // Block-type scheduling from pre-M/S granule energies (M/S preserves
        // total energy, and both channels must share a window type anyway).
        int btypes[2] = { 0, 0 };
        // LSF (MPEG-2/2.5) short blocks use the ISO 13818-3 four-slen
        // scalefac_compress encoding over 3-band groups (see
        // encode_scalefac_fields); the historical 20.8 -> 8.6 dB collapse
        // came from emitting them with the MPEG-1 two-slen layout.
        if (kShortBlocksEnabled && enc->quality_mode >= 1) {
            double gr_energy[3] = { 0.0, 0.0, 0.0 };
            for (int g = 0; g < num_gr; g++)
                for (int ch = 0; ch < nch; ch++)
                    for (int sb = 0; sb < 32; sb++)
                        for (int ts = g * 18; ts < g * 18 + 18; ts++) {
                            double v = subband_out_d[ch][sb][ts];
                            gr_energy[g] += v * v;
                        }
            for (int ch = 0; ch < nch; ch++)
                for (int sb = 0; sb < 32; sb++)
                    for (int ts = 0; ts < 18; ts++) {
                        double v = enc->held_sub_d[ch][sb][ts];
                        gr_energy[num_gr] += v * v;
                    }
            schedule_block_types(enc, gr_energy, num_gr, btypes);
        }

        // Inter-granule bit redistribution for quality_mode >= 2 (best).
        // Compute per-granule energy and allocate more bits to higher-energy
        // granules. This improves SNR by giving more bits where they matter.
        int bits_gr[2] = { bits_per_granule, bits_per_granule };
        if (kGranuleRedistribution && enc->quality_mode >= 2 && num_gr == 2 && !enc->vbr_mode) {
            double energy[2] = {0, 0};
            for (int gr = 0; gr < 2; gr++)
                for (int ch = 0; ch < nch; ch++)
                    for (int sb = 0; sb < 32; sb++)
                        for (int ts = gr*18; ts < (gr+1)*18; ts++)
                            energy[gr] += subband_out_d[ch][sb][ts] * subband_out_d[ch][sb][ts];

            double total = energy[0] + energy[1];
            if (total > 0) {
                // Allocate proportional to energy, with 30%/70% min/max limits
                double ratio0 = std::max(0.3, std::min(0.7, energy[0] / total));
                bits_gr[0] = static_cast<int>(available_bits * ratio0 / nch);
                bits_gr[1] = static_cast<int>(available_bits * (1.0 - ratio0) / nch);
            }
        }

        for (int gr = 0; gr < num_gr; gr++) {
            int t0 = gr * 18;

            if (use_ms) {
                for (int sb = 0; sb < 32; sb++)
                    for (int ts = 0; ts < 18; ts++) {
                        double l = subband_out_d[0][sb][t0 + ts];
                        double r = subband_out_d[1][sb][t0 + ts];
                        subband_out_d[0][sb][t0 + ts] = (l + r) * 0.7071067811865476;
                        subband_out_d[1][sb][t0 + ts] = (l - r) * 0.7071067811865476;
                    }
                mode_ext = 2;
            }

            // Per-channel bit split within the granule: proportional to
            // post-transform energy, clamped, integer-exact (the two shares
            // sum to the old 50/50 total). Matters most for M/S, where the
            // side channel of correlated material needs far fewer bits than
            // mid; a fixed 50/50 split starves mid and wastes side bits.
            int bits_ch[2] = { bits_gr[gr], bits_gr[gr] };
            if (nch == 2) {
                double e[2] = {0, 0};
                for (int ch = 0; ch < 2; ch++)
                    for (int sb = 0; sb < 32; sb++)
                        for (int ts = 0; ts < 18; ts++) {
                            double v = subband_out_d[ch][sb][t0 + ts];
                            e[ch] += v * v;
                        }
                double tot = e[0] + e[1];
                if (tot > 0) {
                    double r0 = std::max(kChSplitLo, std::min(kChSplitHi, e[0] / tot));
                    int total_gr = bits_gr[gr] * 2;
                    bits_ch[0] = static_cast<int>(total_gr * r0);
                    bits_ch[1] = total_gr - bits_ch[0];
                }
            }

            for (int ch = 0; ch < nch; ch++) {
                int gr_bits = bits_ch[ch];

                int bt = btypes[gr];
                if (bt == 2) {
                    // Short-block path needs sub_gr copy (freq inversion)
                    double sub_gr[32][18];
                    for (int sb = 0; sb < 32; sb++)
                        for (int ts = 0; ts < 18; ts++) {
                            double v = subband_out_d[ch][sb][t0 + ts];
                            sub_gr[sb][ts] = ((sb & 1) && (ts & 1)) ? -v : v;
                        }
                    double mdct_out_short[32][3][6];
                    enc->mdct[ch].process_short(sub_gr, mdct_out_short);
                    // No alias reduction for short blocks (ISO spec)

                    // Reorder to flat 576 array
                    double mdct_flat[576];
                    reorder_short_blocks(mdct_out_short, mdct_flat, enc->sr_index);
                    apply_sfb21_lowpass(mdct_flat, enc->sr_index, true);

                    // Quantize with short-block region layout so the gain
                    // search fits the bit budget under the SAME layout the
                    // bitstream actually uses. (Previously the gain was fitted
                    // with the long-block layout and the regions were swapped
                    // afterwards, leaving part2_3_length far over budget and
                    // overflowing the 12-bit side-info field.)
                    if (enc->vbr_mode) {
                        granule_info[gr][ch] = quantize_granule_vbr(mdct_flat, gr_bits,
                            enc->sr_index, enc->quality_mode, enc->vbr_quality,
                            /*block_type=*/2);
                    } else {
                        granule_info[gr][ch] = quantize_granule(mdct_flat, gr_bits,
                                                                 enc->sr_index, enc->quality_mode,
                                                                 /*block_type=*/2,
                                                                 gain_floor,
                                                                 !use_ms || ch == 0);
                    }
                    granule_info[gr][ch].block_type = 2;
                } else {
                    // Long / start / stop path: fused freq inversion + MDCT
                    // with the block type's window. Start/stop granules use
                    // the window-switching side-info layout, so they quantize
                    // under the short region layout (short_block=true).
                    double mdct_out[32][18];
                    enc->mdct[ch].process_strided(subband_out_d[ch], t0,
                                                  mdct_out, bt);
                    alias_reduce_d(mdct_out);

                    double* mdct_flat = &mdct_out[0][0];
                    apply_sfb21_lowpass(mdct_flat, enc->sr_index, false);

                    if (enc->vbr_mode) {
                        granule_info[gr][ch] = quantize_granule_vbr(mdct_flat, gr_bits,
                            enc->sr_index, enc->quality_mode, enc->vbr_quality,
                            /*block_type=*/bt);
                    } else {
                        granule_info[gr][ch] = quantize_granule(mdct_flat, gr_bits,
                                                                 enc->sr_index, enc->quality_mode,
                                                                 /*block_type=*/bt,
                                                                 gain_floor,
                                                                 !use_ms || ch == 0);
                    }
                    granule_info[gr][ch].block_type = bt;
                }

                total_main_bits += granule_info[gr][ch].part2_3_length;
            }
        }
    };
#endif // double-precision path

#ifdef GLINT_FIXED_POINT
    // Helper lambda: encode one frame through the fixed-point Q24 path
    auto encode_fixed = [&]() {
        int32_t subband_out_fp[2][32][36];
        for (int ch = 0; ch < nch; ch++)
            enc->subband_fp[ch].analyze(channel_data[ch], subband_out_fp[ch], num_slots);

        int bits_gr[2] = { bits_per_granule, bits_per_granule };
        if (kGranuleRedistribution && enc->quality_mode >= 2 && num_gr == 2 && !enc->vbr_mode) {
            double energy[2] = {0, 0};
            for (int gr = 0; gr < 2; gr++)
                for (int ch = 0; ch < nch; ch++)
                    for (int sb = 0; sb < 32; sb++)
                        for (int ts = gr*18; ts < (gr+1)*18; ts++) {
                            double v = static_cast<double>(subband_out_fp[ch][sb][ts]);
                            energy[gr] += v * v;
                        }

            double total = energy[0] + energy[1];
            if (total > 0) {
                double ratio0 = std::max(0.3, std::min(0.7, energy[0] / total));
                bits_gr[0] = static_cast<int>(available_bits * ratio0 / nch);
                bits_gr[1] = static_cast<int>(available_bits * (1.0 - ratio0) / nch);
            }
        }

        for (int gr = 0; gr < num_gr; gr++) {
            int t0 = gr * 18;

            if (use_ms) {
                for (int sb = 0; sb < 32; sb++)
                    for (int ts = 0; ts < 18; ts++) {
                        int32_t l = subband_out_fp[0][sb][t0 + ts];
                        int32_t r = subband_out_fp[1][sb][t0 + ts];
                        int64_t sum64 = static_cast<int64_t>(l) + r;
                        int64_t diff64 = static_cast<int64_t>(l) - r;
                        subband_out_fp[0][sb][t0 + ts] = static_cast<int32_t>((sum64 * kInvSqrt2_Q31) >> 31);
                        subband_out_fp[1][sb][t0 + ts] = static_cast<int32_t>((diff64 * kInvSqrt2_Q31) >> 31);
                    }
                mode_ext = 2;
            }

            // Per-channel bit split within the granule (see the double path).
            int bits_ch[2] = { bits_gr[gr], bits_gr[gr] };
            if (nch == 2) {
                double e[2] = {0, 0};
                for (int ch = 0; ch < 2; ch++)
                    for (int sb = 0; sb < 32; sb++)
                        for (int ts = 0; ts < 18; ts++) {
                            double v = static_cast<double>(subband_out_fp[ch][sb][t0 + ts]);
                            e[ch] += v * v;
                        }
                double tot = e[0] + e[1];
                if (tot > 0) {
                    double r0 = std::max(kChSplitLo, std::min(kChSplitHi, e[0] / tot));
                    int total_gr = bits_gr[gr] * 2;
                    bits_ch[0] = static_cast<int>(total_gr * r0);
                    bits_ch[1] = total_gr - bits_ch[0];
                }
            }

            for (int ch = 0; ch < nch; ch++) {
                int gr_bits = bits_ch[ch];

                // Frequency inversion and extract granule slice in one pass
                int32_t sub_gr[32][18];
                for (int sb = 0; sb < 32; sb++)
                    for (int ts = 0; ts < 18; ts++) {
                        int32_t v = subband_out_fp[ch][sb][t0 + ts];
                        sub_gr[sb][ts] = ((sb & 1) && (ts & 1)) ? -v : v;
                    }

                // Fused MDCT + alias reduction + Q24->double conversion.
                // Outputs flat double[576] directly for the quantizer.
                double mdct_flat[576];
                enc->mdct_fp[ch].process_and_convert(sub_gr, mdct_flat);
                apply_sfb21_lowpass(mdct_flat, enc->sr_index, false);

                if (enc->vbr_mode) {
                    granule_info[gr][ch] = quantize_granule_vbr(mdct_flat, gr_bits,
                        enc->sr_index, enc->quality_mode, enc->vbr_quality);
                } else {
                    granule_info[gr][ch] = quantize_granule(mdct_flat, gr_bits,
                                                             enc->sr_index, enc->quality_mode,
                                                             /*block_type=*/0,
                                                             gain_floor,
                                                             !use_ms || ch == 0);
                }
                total_main_bits += granule_info[gr][ch].part2_3_length;
            }
        }
    };
#endif

    // Dispatch to selected signal path
#ifdef GLINT_BOTH_PATHS
    if (enc->use_fixed_point)
        encode_fixed();
    else
        encode_double();
#elif defined(GLINT_FIXED_POINT)
    encode_fixed();
#else
    encode_double();
#endif

    // Rate controller: track the achieved operating point (mean global_gain)
    // so the constant-quality anchor stays tethered to the content.
    if (!enc->vbr_mode) {
        int gsum = 0, gn = 0;
        for (int gr = 0; gr < num_gr; gr++)
            for (int ch = 0; ch < nch; ch++) {
                // rc_gain, not global_gain: psy shaping coarsens the gain
                // to pay for scalefactors, and feeding shaped gains into
                // the EMA ratcheted the anchor floor up (see GranuleInfo).
                gsum += granule_info[gr][ch].rc_gain;
                gn++;
            }
        int gmean = (gn > 0) ? gsum / gn : 0;
        if (gmean > 0) {
            if (enc->rc_gain_ema_x16 == 0) enc->rc_gain_ema_x16 = gmean * 16;
            else enc->rc_gain_ema_x16 += (gmean * 16 - enc->rc_gain_ema_x16) / 8;
            if (enc->rc_anchor == 0) enc->rc_anchor = gmean;
        }
    }

    return finish_frame(enc, granule_info, num_gr, nch, mode_ext,
                        this_frame_size, total_main_bits, out_size);
}

const uint8_t* glint_encode_float(glint_t enc, const float** channel_data,
                                  int* out_size) {
    if (!enc || !channel_data || !out_size) {
        if (out_size) *out_size = 0;
        return nullptr;
    }

    int nch = enc->num_channels;
    bool use_ms = (enc->config.mode == GLINT_JOINT && nch == 2);

    // If using fixed-point path, fall back to int16 conversion (float path
    // only applies to the double-precision signal path).
    if (enc->use_fixed_point) {
        int spf = glint_samples_per_frame(enc);
        int16_t* bufs[2];
        bufs[0] = new int16_t[spf];
        bufs[1] = (nch > 1) ? new int16_t[spf] : nullptr;
        for (int ch = 0; ch < nch; ch++) {
            for (int i = 0; i < spf; i++) {
                float v = channel_data[ch][i] * 32767.0f;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                bufs[ch][i] = static_cast<int16_t>(v);
            }
        }
        const int16_t* ptrs[2] = { bufs[0], bufs[1] };
        const uint8_t* result = glint_encode(enc, ptrs, out_size);
        delete[] bufs[0];
        delete[] bufs[1];
        return result;
    }

#if !defined(GLINT_FIXED_POINT) || defined(GLINT_BOTH_PATHS)
    // --- Double-precision float path: feed float directly into subband analysis ---

    // Padding
    enc->padding = 0;
    int bitrate_bps = enc->config.bitrate * 1000;
    int frame_slot_mult = (enc->mpeg_version == 1) ? 144 : 72;
    int remainder = (frame_slot_mult * bitrate_bps) % enc->config.sample_rate;
    enc->padding_remainder += remainder;
    if (enc->padding_remainder >= enc->config.sample_rate) {
        enc->padding_remainder -= enc->config.sample_rate;
        enc->padding = 1;
    }

    // VBR frames are emitted unpadded (vbr_pick_frame_size), so the
    // quantization budget must not assume the padding byte — otherwise a
    // full frame can need one byte more than the largest emitted frame.
    int this_frame_size = enc->frame_size + (enc->vbr_mode ? 0 : enc->padding);
    int this_frame_bits = this_frame_size * 8 - 32 - enc->side_info_bits;

    // Bit reservoir rate control (CBR): a frame may spend its own slot plus
    // the reservoir (capped at one extra slot), but a per-frame gain FLOOR —
    // the constant-quality anchor — stops the quantizer from spending finer
    // than the current quality target, so easy frames bank bits instead of
    // gold-plating. The anchor adapts +-1 gain step per frame from reservoir
    // fill (>60% full: aim finer / spend; <40%: aim coarser / save) and is
    // tethered to the EMA of achieved gains so it tracks the content's
    // operating point. This replaces the measured-bad naive policy ("every
    // frame may spend the whole reservoir", which oscillated); see PLAN.md
    // item 5.
    int available_bits = this_frame_bits;
    int gain_floor = 0;
    if (!enc->vbr_mode) {
        int mdb_bits = 8 * enc->reservoir.main_data_begin();
        available_bits += std::min(mdb_bits, this_frame_bits);
        if (enc->rc_anchor > 0) {
            int resv_cap_bits = 8 * (enc->mpeg_version == 1 ? 511 : 255);
            if (mdb_bits * 10 > resv_cap_bits * 6) enc->rc_anchor--;
            else if (mdb_bits * 10 < resv_cap_bits * 4) enc->rc_anchor++;
            int ema = enc->rc_gain_ema_x16 / 16;
            if (enc->rc_anchor < ema - 12) enc->rc_anchor = ema - 12;
            if (enc->rc_anchor > ema + 12) enc->rc_anchor = ema + 12;
            if (enc->rc_anchor < 1) enc->rc_anchor = 1;
            if (enc->rc_anchor > 255) enc->rc_anchor = 255;
            gain_floor = enc->rc_anchor;
        }
    }

    int num_gr = enc->num_granules;
    int bits_per_granule = available_bits / (num_gr * nch);
    if (bits_per_granule < 0) bits_per_granule = 0;

    int num_slots = num_gr * 18;
    GranuleInfo granule_info[2][2];
    int total_main_bits = 0;
    int mode_ext = 0;

    // Subband analysis using float input (no int16 truncation)
    double sb_new[2][32][36];
    for (int ch = 0; ch < nch; ch++)
        enc->subband[ch].analyze_float(channel_data[ch], sb_new[ch], num_slots);

    // One-granule encoder delay: the emitted frame is the held granule
    // followed by this call's granules except the last, which becomes the
    // new held (lookahead) granule. This is what lets the scheduler place
    // a START window on the granule BEFORE every transient. glint_flush
    // releases the final held granule.
    double subband_out_d[2][32][36];
    for (int ch = 0; ch < nch; ch++)
        for (int sb = 0; sb < 32; sb++) {
            for (int ts = 0; ts < 18; ts++)
                subband_out_d[ch][sb][ts] =
                    enc->have_held ? enc->held_sub_d[ch][sb][ts] : 0.0;
            for (int g = 1; g < num_gr; g++)
                for (int ts = 0; ts < 18; ts++)
                    subband_out_d[ch][sb][g * 18 + ts] =
                        sb_new[ch][sb][(g - 1) * 18 + ts];
            for (int ts = 0; ts < 18; ts++)
                enc->held_sub_d[ch][sb][ts] =
                    sb_new[ch][sb][(num_gr - 1) * 18 + ts];
        }
    enc->have_held = true;

    // Block-type scheduling from pre-M/S granule energies (M/S preserves
    // total energy, and both channels must share a window type anyway).
    int btypes[2] = { 0, 0 };
    // LSF short blocks: see the note in glint_encode.
    if (kShortBlocksEnabled && enc->quality_mode >= 1) {
        double gr_energy[3] = { 0.0, 0.0, 0.0 };
        for (int g = 0; g < num_gr; g++)
            for (int ch = 0; ch < nch; ch++)
                for (int sb = 0; sb < 32; sb++)
                    for (int ts = g * 18; ts < g * 18 + 18; ts++) {
                        double v = subband_out_d[ch][sb][ts];
                        gr_energy[g] += v * v;
                    }
        for (int ch = 0; ch < nch; ch++)
            for (int sb = 0; sb < 32; sb++)
                for (int ts = 0; ts < 18; ts++) {
                    double v = enc->held_sub_d[ch][sb][ts];
                    gr_energy[num_gr] += v * v;
                }
        schedule_block_types(enc, gr_energy, num_gr, btypes);
    }

    int bits_gr[2] = { bits_per_granule, bits_per_granule };
    if (kGranuleRedistribution && enc->quality_mode >= 2 && num_gr == 2 && !enc->vbr_mode) {
        double energy[2] = {0, 0};
        for (int gr = 0; gr < 2; gr++)
            for (int sb = 0; sb < 32; sb++)
                for (int ts = gr*18; ts < (gr+1)*18; ts++)
                    energy[gr] += subband_out_d[0][sb][ts] * subband_out_d[0][sb][ts];

        double total = energy[0] + energy[1];
        if (total > 0) {
            double ratio0 = std::max(0.3, std::min(0.7, energy[0] / total));
            bits_gr[0] = static_cast<int>(available_bits * ratio0 / nch);
            bits_gr[1] = static_cast<int>(available_bits * (1.0 - ratio0) / nch);
        }
    }

    for (int gr = 0; gr < num_gr; gr++) {
        int t0 = gr * 18;

        if (use_ms) {
            for (int sb = 0; sb < 32; sb++)
                for (int ts = 0; ts < 18; ts++) {
                    double l = subband_out_d[0][sb][t0 + ts];
                    double r = subband_out_d[1][sb][t0 + ts];
                    subband_out_d[0][sb][t0 + ts] = (l + r) * 0.7071067811865476;
                    subband_out_d[1][sb][t0 + ts] = (l - r) * 0.7071067811865476;
                }
            mode_ext = 2;
        }

        for (int ch = 0; ch < nch; ch++) {
            int gr_bits = bits_gr[gr];

            int bt = btypes[gr];
            if (bt == 2) {
                double sub_gr[32][18];
                for (int sb = 0; sb < 32; sb++)
                    for (int ts = 0; ts < 18; ts++) {
                        double v = subband_out_d[ch][sb][t0 + ts];
                        sub_gr[sb][ts] = ((sb & 1) && (ts & 1)) ? -v : v;
                    }
                double mdct_out_short[32][3][6];
                enc->mdct[ch].process_short(sub_gr, mdct_out_short);

                double mdct_flat[576];
                reorder_short_blocks(mdct_out_short, mdct_flat, enc->sr_index);
                apply_sfb21_lowpass(mdct_flat, enc->sr_index, true);

                if (enc->vbr_mode) {
                    granule_info[gr][ch] = quantize_granule_vbr(mdct_flat, gr_bits,
                        enc->sr_index, enc->quality_mode, enc->vbr_quality,
                        /*block_type=*/2);
                } else {
                    granule_info[gr][ch] = quantize_granule(mdct_flat, gr_bits,
                                                             enc->sr_index, enc->quality_mode,
                                                             /*block_type=*/2,
                                                             gain_floor,
                                                             !use_ms || ch == 0);
                }
                granule_info[gr][ch].block_type = 2;
            } else {
                double mdct_out[32][18];
                enc->mdct[ch].process_strided(subband_out_d[ch], t0, mdct_out,
                                              bt);
                alias_reduce_d(mdct_out);

                double* mdct_flat = &mdct_out[0][0];
                apply_sfb21_lowpass(mdct_flat, enc->sr_index, false);

                if (enc->vbr_mode) {
                    granule_info[gr][ch] = quantize_granule_vbr(mdct_flat, gr_bits,
                        enc->sr_index, enc->quality_mode, enc->vbr_quality,
                        /*block_type=*/bt);
                } else {
                    granule_info[gr][ch] = quantize_granule(mdct_flat, gr_bits,
                                                             enc->sr_index, enc->quality_mode,
                                                             /*block_type=*/bt,
                                                             gain_floor,
                                                             !use_ms || ch == 0);
                }
                granule_info[gr][ch].block_type = bt;
            }

            total_main_bits += granule_info[gr][ch].part2_3_length;
        }
    }

    // --- Frame assembly (identical to glint_encode) ---

    // Rate controller: track the achieved operating point (mean global_gain)
    // so the constant-quality anchor stays tethered to the content.
    if (!enc->vbr_mode) {
        int gsum = 0, gn = 0;
        for (int gr = 0; gr < num_gr; gr++)
            for (int ch = 0; ch < nch; ch++) {
                // rc_gain, not global_gain: psy shaping coarsens the gain
                // to pay for scalefactors, and feeding shaped gains into
                // the EMA ratcheted the anchor floor up (see GranuleInfo).
                gsum += granule_info[gr][ch].rc_gain;
                gn++;
            }
        int gmean = (gn > 0) ? gsum / gn : 0;
        if (gmean > 0) {
            if (enc->rc_gain_ema_x16 == 0) enc->rc_gain_ema_x16 = gmean * 16;
            else enc->rc_gain_ema_x16 += (gmean * 16 - enc->rc_gain_ema_x16) / 8;
            if (enc->rc_anchor == 0) enc->rc_anchor = gmean;
        }
    }

    return finish_frame(enc, granule_info, num_gr, nch, mode_ext,
                        this_frame_size, total_main_bits, out_size);
#else
    // Pure fixed-point build: should never reach here (handled above)
    *out_size = 0;
    return nullptr;
#endif // double-precision float path
}

const uint8_t* glint_encode_int32(glint_t enc, const int32_t** channel_data,
                                   int* out_size) {
    if (!enc || !channel_data || !out_size) {
        if (out_size) *out_size = 0;
        return nullptr;
    }
    int spf = glint_samples_per_frame(enc);
    int nch = enc->num_channels;
    int16_t* bufs[2];
    bufs[0] = new int16_t[spf];
    bufs[1] = (nch > 1) ? new int16_t[spf] : nullptr;
    for (int ch = 0; ch < nch; ch++) {
        for (int i = 0; i < spf; i++)
            bufs[ch][i] = static_cast<int16_t>(channel_data[ch][i] >> 16);
    }
    const int16_t* ptrs[2] = { bufs[0], bufs[1] };
    const uint8_t* result = glint_encode(enc, ptrs, out_size);
    delete[] bufs[0];
    delete[] bufs[1];
    return result;
}

const uint8_t* glint_flush(glint_t enc, int* out_size) {
    if (!enc || !out_size) {
        if (out_size) *out_size = 0;
        return nullptr;
    }
    int total = 0;

#if !defined(GLINT_FIXED_POINT) || defined(GLINT_BOTH_PATHS)
    // Release the one-granule lookahead: encode a final frame of silence so
    // the held granule is emitted (the granule of silence it holds back in
    // turn is dropped). The callback, if any, fires inside glint_encode.
    if (enc->have_held && !enc->use_fixed_point) {
        int spf = glint_samples_per_frame(enc);
        int16_t* silence = new int16_t[spf]();
        const int16_t* ptrs[2] = { silence, silence };
        int sz = 0;
        glint_encode(enc, ptrs, &sz);
        delete[] silence;
        enc->have_held = false;
        total = sz;  // already at the start of output_buf
    }
#endif

    // Release the reservoir's buffered frames (CBR; VBR frames are emitted
    // immediately and there is nothing to drain).
    if (!enc->vbr_mode) {
        int drained = enc->reservoir.flush(
            enc->output_buf + total,
            static_cast<int>(sizeof(enc->output_buf)) - total);
        if (enc->write_cb && drained > 0)
            enc->write_cb(enc->output_buf + total, drained, enc->write_cb_data);
        total += drained;
    }
    *out_size = total;
    return enc->output_buf;
}

void glint_destroy(glint_t enc) {
    delete enc;
}
