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

// Transient detection: compares energy between consecutive granules.
// Returns true if a transient (>9 dB energy jump) is detected.
static bool detect_transient(const double subband_out[32][36], int gr,
                             bool prev_energy_valid, double* prev_energy) {
    double energy = 0;
    int t0 = gr * 18;
    for (int sb = 0; sb < 32; sb++)
        for (int ts = t0; ts < t0 + 18; ts++)
            energy += subband_out[sb][ts] * subband_out[sb][ts];

    bool transient = false;
    if (prev_energy_valid && *prev_energy > 0) {
        double ratio = energy / *prev_energy;
        if (ratio > 8.0) transient = true;  // 9 dB jump = transient
    }
    *prev_energy = energy;
    return transient;
}
#endif // double-precision path helpers

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

glint_t glint_create(const glint_config* cfg) {
    if (!cfg) return nullptr;

    // For VBR, use 320 kbps frame size (max bitrate) regardless of -b
    int effective_bitrate = cfg->bitrate;
    if (cfg->vbr == GLINT_VBR_ON) {
        effective_bitrate = 320;
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

    if (ctx->mpeg_version == 1) {
        ctx->br_index = tables::bitrate_to_index(cfg->bitrate);
    } else {
        ctx->br_index = tables::bitrate_to_index_m2(cfg->bitrate);
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

    int bitrate_bps = cfg->bitrate * 1000;
    if (ctx->mpeg_version == 1) {
        ctx->frame_size = 144 * bitrate_bps / cfg->sample_rate;
    } else {
        ctx->frame_size = 72 * bitrate_bps / cfg->sample_rate;
    }
    ctx->padding_threshold = cfg->sample_rate;
    ctx->padding_remainder = 0;

    int total_frame_bits = ctx->frame_size * 8;
    ctx->mean_bits_per_frame = total_frame_bits - 32 - ctx->side_info_bits;

    ctx->reservoir.init(ctx->mean_bits_per_frame);
    ctx->reservoir_buf_write = 0;
    ctx->reservoir_buf_size = 0;

    // Determine signal path
#ifdef GLINT_BOTH_PATHS
    ctx->use_fixed_point = (cfg->path != GLINT_PATH_DOUBLE);
#elif defined(GLINT_FIXED_POINT)
    ctx->use_fixed_point = true;
#else
    ctx->use_fixed_point = false;
#endif

    ctx->prev_granule_energy[0] = 0;
    ctx->prev_granule_energy[1] = 0;
    ctx->prev_energy_valid = false;

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
        // subblock_gain[0..2] (3 x 3 bits)
        bs.write_bits(0, 3); bs.write_bits(0, 3); bs.write_bits(0, 3);
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

    int this_frame_size = enc->frame_size + enc->padding;
    int this_frame_bits = this_frame_size * 8 - 32 - enc->side_info_bits;

    // Bit reservoir disabled — each frame is self-contained.
    // The reservoir implementation has backstep alignment issues; disabled
    // until properly debugged.
    int reservoir_bytes = 0;
    int available_bits = this_frame_bits;

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
        double subband_out_d[2][32][36];
        for (int ch = 0; ch < nch; ch++)
            enc->subband[ch].analyze(channel_data[ch], subband_out_d[ch], num_slots);

        // Inter-granule bit redistribution for quality_mode >= 2 (best).
        // Compute per-granule energy and allocate more bits to higher-energy
        // granules. This improves SNR by giving more bits where they matter.
        int bits_gr[2] = { bits_per_granule, bits_per_granule };
        if (enc->quality_mode >= 2 && num_gr == 2 && !enc->vbr_mode) {
            double energy[2] = {0, 0};
            for (int gr = 0; gr < 2; gr++)
                for (int sb = 0; sb < 32; sb++)
                    for (int ts = gr*18; ts < (gr+1)*18; ts++)
                        energy[gr] += subband_out_d[0][sb][ts] * subband_out_d[0][sb][ts];

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

            for (int ch = 0; ch < nch; ch++) {
                int gr_bits = bits_gr[gr];

                // Transient detection on subband output
                bool transient_detected = detect_transient(
                    subband_out_d[ch], gr,
                    enc->prev_energy_valid, &enc->prev_granule_energy[ch]);

                // Frequency inversion and extract granule slice in one pass
                double sub_gr[32][18];
                for (int sb = 0; sb < 32; sb++)
                    for (int ts = 0; ts < 18; ts++) {
                        double v = subband_out_d[ch][sb][t0 + ts];
                        sub_gr[sb][ts] = ((sb & 1) && (ts & 1)) ? -v : v;
                    }

                bool use_short = transient_detected && enc->quality_mode >= 1;

                if (use_short) {
                    // Short-block path
                    double mdct_out_short[32][3][6];
                    enc->mdct[ch].process_short(sub_gr, mdct_out_short);
                    // No alias reduction for short blocks (ISO spec)

                    // Reorder to flat 576 array
                    double mdct_flat[576];
                    reorder_short_blocks(mdct_out_short, mdct_flat, enc->sr_index);

                    // Quantize with short-block region layout so the gain
                    // search fits the bit budget under the SAME layout the
                    // bitstream actually uses. (Previously the gain was fitted
                    // with the long-block layout and the regions were swapped
                    // afterwards, leaving part2_3_length far over budget and
                    // overflowing the 12-bit side-info field.)
                    if (enc->vbr_mode) {
                        granule_info[gr][ch] = quantize_granule_vbr(mdct_flat,
                            enc->sr_index, enc->quality_mode, enc->vbr_quality,
                            /*short_block=*/true);
                    } else {
                        granule_info[gr][ch] = quantize_granule(mdct_flat, gr_bits,
                                                                 enc->sr_index, enc->quality_mode,
                                                                 /*short_block=*/true);
                    }
                    granule_info[gr][ch].block_type = 2;
                } else {
                    // Long-block path
                    double mdct_out[32][18];
                    enc->mdct[ch].process(sub_gr, mdct_out);
                    alias_reduce_d(mdct_out);

                    double mdct_flat[576];
                    for (int sb = 0; sb < 32; sb++)
                        for (int k = 0; k < 18; k++)
                            mdct_flat[sb * 18 + k] = mdct_out[sb][k];

                    if (enc->vbr_mode) {
                        granule_info[gr][ch] = quantize_granule_vbr(mdct_flat,
                            enc->sr_index, enc->quality_mode, enc->vbr_quality);
                    } else {
                        granule_info[gr][ch] = quantize_granule(mdct_flat, gr_bits,
                                                                 enc->sr_index, enc->quality_mode);
                    }
                    granule_info[gr][ch].block_type = 0;
                }

                total_main_bits += granule_info[gr][ch].part2_3_length;
            }
        }
        // Mark energy as valid after first frame
        enc->prev_energy_valid = true;
    };
#endif // double-precision path

#ifdef GLINT_FIXED_POINT
    // Helper lambda: encode one frame through the fixed-point Q24 path
    auto encode_fixed = [&]() {
        int32_t subband_out_fp[2][32][36];
        for (int ch = 0; ch < nch; ch++)
            enc->subband_fp[ch].analyze(channel_data[ch], subband_out_fp[ch], num_slots);

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

            for (int ch = 0; ch < nch; ch++) {
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

                if (enc->vbr_mode) {
                    granule_info[gr][ch] = quantize_granule_vbr(mdct_flat,
                        enc->sr_index, enc->quality_mode, enc->vbr_quality);
                } else {
                    granule_info[gr][ch] = quantize_granule(mdct_flat, bits_per_granule,
                                                             enc->sr_index, enc->quality_mode);
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

    // Compute SCFSI: share scalefactors between granules when identical (MPEG-1 only)
    // Groups: 0=bands 0-5, 1=bands 6-10, 2=bands 11-15, 3=bands 16-20
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
                // Group 0 (bands 0-5) and Group 1 (bands 6-10) use slen1
                if (scfsi[ch][0]) saved += slen1 * 6;
                if (scfsi[ch][1]) saved += slen1 * 5;
                // Group 2 (bands 11-15) and Group 3 (bands 16-20) use slen2
                if (scfsi[ch][2]) saved += slen2 * 5;
                if (scfsi[ch][3]) saved += slen2 * 5;
                granule_info[1][ch].part2_length -= saved;
                granule_info[1][ch].part2_3_length -= saved;
                total_main_bits -= saved;
            }
        }
    }

    // Write frame header
    int channel_mode = mode_to_mpeg(enc->config.mode);
    // The header needs the 2-bit sample rate index (0-2), not the unified one
    int hdr_sr_index = (enc->sr_index < 3) ? enc->sr_index : (enc->sr_index - 3);
    enc->frame_asm.reset();
    enc->frame_asm.write_header(enc->br_index, hdr_sr_index,
                                 enc->padding, channel_mode, mode_ext,
                                 enc->mpeg_version);

    // Write side information
    BitstreamWriter& si = enc->frame_asm.side_info();
    si.reset();
    int main_data_begin = std::min(reservoir_bytes, enc->reservoir_buf_size);

    if (enc->mpeg_version == 1) {
        // MPEG-1 side info
        si.write_bits(main_data_begin, 9);
        if (nch == 1) si.write_bits(0, 5); else si.write_bits(0, 3); // private bits
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
        // MPEG-2/2.5 side info
        si.write_bits(main_data_begin, 8); // 8 bits for MPEG-2
        if (nch == 1) si.write_bits(0, 1); else si.write_bits(0, 2); // private bits
        // No scfsi for MPEG-2
        for (int ch = 0; ch < nch; ch++)
            write_granule_side_info(si, granule_info[0][ch], enc->mpeg_version);
    }

    // Write main data
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
                // Range 0..179: slen[0]=sfc/36, slen[1]=(sfc%36)/6, slen[2]=(sfc%36)%6, slen[3]=0
                //   band groups: [6, 5, 5, 5]
                int sfc = gi.scalefac_compress;
                int slen[4] = {};
                int nr[4] = {6, 5, 5, 5};
                if (sfc < 180) {
                    slen[0] = sfc / 36;
                    slen[1] = (sfc % 36) / 6;
                    slen[2] = (sfc % 36) % 6;
                    slen[3] = 0;
                } else if (sfc < 244) {
                    int v = sfc - 180;
                    slen[0] = v / 16;
                    slen[1] = (v % 16) / 4;
                    slen[2] = v % 4;
                    slen[3] = 0;
                    nr[0] = 6; nr[1] = 5; nr[2] = 7; nr[3] = 3;
                } else {
                    int v = sfc - 244;
                    slen[0] = v / 3;
                    slen[1] = v % 3;
                    slen[2] = 0;
                    slen[3] = 0;
                    nr[0] = 11; nr[1] = 10; nr[2] = 0; nr[3] = 0;
                }
                // Write scalefactors in 4 groups
                int b = 0;
                for (int g = 0; g < 4; g++) {
                    for (int i = 0; i < nr[g] && b < 21; i++, b++) {
                        if (slen[g] > 0)
                            md.write_bits(gi.scalefac[b], slen[g]);
                    }
                }
            } else {
                // MPEG-1 scalefactor encoding
                int slen1 = slen_table_m1[gi.scalefac_compress][0];
                int slen2 = slen_table_m1[gi.scalefac_compress][1];

                if (gr == 0) {
                    // Granule 0: always write all scalefactors
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

    // Flush main data writer and get the encoded main data bytes
    md.flush();
    int md_bytes = md.byte_count();

    // Update bit reservoir with actual usage
    enc->reservoir.update(total_main_bits);

    // Assemble frame: header + side_info + main_data + zero padding
    const uint8_t* frame = enc->frame_asm.assemble(this_frame_size, out_size);
    std::memcpy(enc->output_buf, frame, *out_size);
    enc->reservoir_buf_size = std::min(enc->reservoir_buf_size + md_bytes, 8192);

    enc->frame_count++;

    // Invoke streaming callback if set
    if (enc->write_cb && *out_size > 0) {
        enc->write_cb(enc->output_buf, *out_size, enc->write_cb_data);
    }

    return enc->output_buf;
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

    int this_frame_size = enc->frame_size + enc->padding;
    int this_frame_bits = this_frame_size * 8 - 32 - enc->side_info_bits;

    int reservoir_bytes = 0;
    int available_bits = this_frame_bits;

    int num_gr = enc->num_granules;
    int bits_per_granule = available_bits / (num_gr * nch);
    if (bits_per_granule < 0) bits_per_granule = 0;

    int num_slots = num_gr * 18;
    GranuleInfo granule_info[2][2];
    int total_main_bits = 0;
    int mode_ext = 0;

    // Subband analysis using float input (no int16 truncation)
    double subband_out_d[2][32][36];
    for (int ch = 0; ch < nch; ch++)
        enc->subband[ch].analyze_float(channel_data[ch], subband_out_d[ch], num_slots);

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
            // Transient detection on subband output
            bool transient_detected = detect_transient(
                subband_out_d[ch], gr,
                enc->prev_energy_valid, &enc->prev_granule_energy[ch]);

            double sub_gr[32][18];
            for (int sb = 0; sb < 32; sb++)
                for (int ts = 0; ts < 18; ts++) {
                    double v = subband_out_d[ch][sb][t0 + ts];
                    sub_gr[sb][ts] = ((sb & 1) && (ts & 1)) ? -v : v;
                }

            bool use_short = transient_detected && enc->quality_mode >= 1;

            if (use_short) {
                double mdct_out_short[32][3][6];
                enc->mdct[ch].process_short(sub_gr, mdct_out_short);

                double mdct_flat[576];
                reorder_short_blocks(mdct_out_short, mdct_flat, enc->sr_index);

                // Short-block quantization with the matching region layout
                // (see glint_encode for the rationale).
                if (enc->vbr_mode) {
                    granule_info[gr][ch] = quantize_granule_vbr(mdct_flat,
                        enc->sr_index, enc->quality_mode, enc->vbr_quality,
                        /*short_block=*/true);
                } else {
                    granule_info[gr][ch] = quantize_granule(mdct_flat, bits_per_granule,
                                                             enc->sr_index, enc->quality_mode,
                                                             /*short_block=*/true);
                }
                granule_info[gr][ch].block_type = 2;
            } else {
                double mdct_out[32][18];
                enc->mdct[ch].process(sub_gr, mdct_out);
                alias_reduce_d(mdct_out);

                double mdct_flat[576];
                for (int sb = 0; sb < 32; sb++)
                    for (int k = 0; k < 18; k++)
                        mdct_flat[sb * 18 + k] = mdct_out[sb][k];

                if (enc->vbr_mode) {
                    granule_info[gr][ch] = quantize_granule_vbr(mdct_flat,
                        enc->sr_index, enc->quality_mode, enc->vbr_quality);
                } else {
                    granule_info[gr][ch] = quantize_granule(mdct_flat, bits_per_granule,
                                                             enc->sr_index, enc->quality_mode);
                }
                granule_info[gr][ch].block_type = 0;
            }

            total_main_bits += granule_info[gr][ch].part2_3_length;
        }
    }
    // Mark energy as valid after first frame
    enc->prev_energy_valid = true;

    // --- Frame assembly (identical to glint_encode) ---

    // Compute SCFSI
    static const int scfsi_band[5] = {0, 6, 11, 16, 21};
    int scfsi[2][4] = {};
    if (num_gr == 2) {
        for (int ch = 0; ch < nch; ch++) {
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

    int channel_mode = mode_to_mpeg(enc->config.mode);
    int hdr_sr_index = (enc->sr_index < 3) ? enc->sr_index : (enc->sr_index - 3);
    enc->frame_asm.reset();
    enc->frame_asm.write_header(enc->br_index, hdr_sr_index,
                                 enc->padding, channel_mode, mode_ext,
                                 enc->mpeg_version);

    BitstreamWriter& si = enc->frame_asm.side_info();
    si.reset();
    int main_data_begin = std::min(reservoir_bytes, enc->reservoir_buf_size);

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

    BitstreamWriter& md = enc->frame_asm.main_data();
    md.reset();

    static const int slen_table_m1f[16][2] = {
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
                if (sfc < 180) {
                    slen[0] = sfc / 36;
                    slen[1] = (sfc % 36) / 6;
                    slen[2] = (sfc % 36) % 6;
                    slen[3] = 0;
                } else if (sfc < 244) {
                    int v = sfc - 180;
                    slen[0] = v / 16;
                    slen[1] = (v % 16) / 4;
                    slen[2] = v % 4;
                    slen[3] = 0;
                    nr[0] = 6; nr[1] = 5; nr[2] = 7; nr[3] = 3;
                } else {
                    int v = sfc - 244;
                    slen[0] = v / 3;
                    slen[1] = v % 3;
                    slen[2] = 0;
                    slen[3] = 0;
                    nr[0] = 11; nr[1] = 10; nr[2] = 0; nr[3] = 0;
                }
                int b = 0;
                for (int g = 0; g < 4; g++) {
                    for (int i = 0; i < nr[g] && b < 21; i++, b++) {
                        if (slen[g] > 0)
                            md.write_bits(gi.scalefac[b], slen[g]);
                    }
                }
            } else {
                int slen1 = slen_table_m1f[gi.scalefac_compress][0];
                int slen2 = slen_table_m1f[gi.scalefac_compress][1];

                if (gr == 0) {
                    for (int b = 0; b < 11; b++)
                        if (slen1 > 0) md.write_bits(gi.scalefac[b], slen1);
                    for (int b = 11; b < 21; b++)
                        if (slen2 > 0) md.write_bits(gi.scalefac[b], slen2);
                } else {
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
    int md_bytes = md.byte_count();

    enc->reservoir.update(total_main_bits);

    const uint8_t* frame = enc->frame_asm.assemble(this_frame_size, out_size);
    std::memcpy(enc->output_buf, frame, *out_size);
    enc->reservoir_buf_size = std::min(enc->reservoir_buf_size + md_bytes, 8192);

    enc->frame_count++;

    // Invoke streaming callback if set
    if (enc->write_cb && *out_size > 0) {
        enc->write_cb(enc->output_buf, *out_size, enc->write_cb_data);
    }

    return enc->output_buf;
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
    *out_size = 0;
    return enc->output_buf;
}

void glint_destroy(glint_t enc) {
    delete enc;
}
