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
    if (glint_check_config(cfg->sample_rate, cfg->bitrate) != 0) return nullptr;
    if (cfg->num_channels < 1 || cfg->num_channels > 2) return nullptr;
    if (cfg->num_channels == 1 && cfg->mode != GLINT_MONO) return nullptr;
    if (cfg->num_channels == 2 && cfg->mode == GLINT_MONO) return nullptr;

    tables::init_tables();
    init_simd(cfg->simd);

    auto* ctx = new (std::nothrow) glint_context{};
    if (!ctx) return nullptr;

    ctx->config = *cfg;
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

    for (int ch = 0; ch < ctx->num_channels; ch++) {
        ctx->subband[ch].reset();
        ctx->mdct[ch].reset();
#ifdef GLINT_FIXED_POINT
        ctx->subband_fp[ch].reset();
        ctx->mdct_fp[ch].reset();
#endif
    }

    return ctx;
}

int glint_samples_per_frame(glint_t enc) {
    if (enc && enc->mpeg_version != 1) {
        return 576;  // MPEG-2/2.5: 1 granule = 576 samples
    }
    return 1152;  // MPEG-1: 2 granules = 1152 samples
}

static void write_granule_side_info(BitstreamWriter& bs, const GranuleInfo& gi) {
    bs.write_bits(gi.part2_3_length, 12);
    bs.write_bits(gi.regions.big_values, 9);
    bs.write_bits(gi.global_gain, 8);
    bs.write_bits(gi.scalefac_compress, 4);
    bs.write_bits(0, 1);  // window_switching_flag = 0 (long blocks)
    bs.write_bits(gi.regions.table_select[0], 5);
    bs.write_bits(gi.regions.table_select[1], 5);
    bs.write_bits(gi.regions.table_select[2], 5);
    bs.write_bits(gi.regions.region0_count, 4);
    bs.write_bits(gi.regions.region1_count, 3);
    bs.write_bits(gi.preflag, 1);
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

    // Helper lambda: encode one frame through the double-precision path
    auto encode_double = [&]() {
        double subband_out_d[2][32][36];
        for (int ch = 0; ch < nch; ch++)
            enc->subband[ch].analyze(channel_data[ch], subband_out_d[ch], num_slots);

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
                // Frequency inversion and extract granule slice in one pass
                double sub_gr[32][18];
                for (int sb = 0; sb < 32; sb++)
                    for (int ts = 0; ts < 18; ts++) {
                        double v = subband_out_d[ch][sb][t0 + ts];
                        sub_gr[sb][ts] = ((sb & 1) && (ts & 1)) ? -v : v;
                    }

                double mdct_out[32][18];
                enc->mdct[ch].process(sub_gr, mdct_out);
                alias_reduce_d(mdct_out);

                double mdct_flat[576];
                for (int sb = 0; sb < 32; sb++)
                    for (int k = 0; k < 18; k++)
                        mdct_flat[sb * 18 + k] = mdct_out[sb][k];

                granule_info[gr][ch] = quantize_granule(mdct_flat, bits_per_granule,
                                                         enc->sr_index);
                total_main_bits += granule_info[gr][ch].part2_3_length;
            }
        }
    };

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

                int32_t mdct_out[32][18];
                enc->mdct_fp[ch].process(sub_gr, mdct_out);
                alias_reduce_fp(mdct_out);

                double mdct_flat[576];
                for (int sb = 0; sb < 32; sb++)
                    for (int k = 0; k < 18; k++)
                        mdct_flat[sb * 18 + k] = mdct_out[sb][k] / 16777216.0;

                granule_info[gr][ch] = quantize_granule(mdct_flat, bits_per_granule,
                                                         enc->sr_index);
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
            for (int group = 0; group < 4; group++) {
                bool match = true;
                for (int b = scfsi_band[group]; b < scfsi_band[group+1] && b < 21; b++) {
                    if (granule_info[0][ch].scalefac[b] != granule_info[1][ch].scalefac[b]) {
                        match = false;
                        break;
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
                write_granule_side_info(si, granule_info[gr][ch]);
    } else {
        // MPEG-2/2.5 side info
        si.write_bits(main_data_begin, 8); // 8 bits for MPEG-2
        if (nch == 1) si.write_bits(0, 1); else si.write_bits(0, 2); // private bits
        // No scfsi for MPEG-2
        for (int ch = 0; ch < nch; ch++)
            write_granule_side_info(si, granule_info[0][ch]);
    }

    // Write main data
    BitstreamWriter& md = enc->frame_asm.main_data();
    md.reset();

    static const int slen_table[16][2] = {
        {0,0},{0,1},{0,2},{0,3},{3,0},{1,1},{1,2},{1,3},
        {2,1},{2,2},{2,3},{3,1},{3,2},{3,3},{4,2},{4,3}
    };

    for (int gr = 0; gr < num_gr; gr++) {
        for (int ch = 0; ch < nch; ch++) {
            const GranuleInfo& gi = granule_info[gr][ch];
            int slen1 = slen_table[gi.scalefac_compress][0];
            int slen2 = slen_table[gi.scalefac_compress][1];

            // Write scalefactors, skipping SCFSI-shared groups for granule 1 (MPEG-1 only)
            if (enc->mpeg_version != 1 || gr == 0) {
                // MPEG-2 or Granule 0: always write all scalefactors
                for (int b = 0; b < 11; b++)
                    if (slen1 > 0) md.write_bits(gi.scalefac[b], slen1);
                for (int b = 11; b < 21; b++)
                    if (slen2 > 0) md.write_bits(gi.scalefac[b], slen2);
            } else {
                // MPEG-1 Granule 1: skip groups where scfsi=1
                for (int b = 0; b < 6; b++)
                    if (slen1 > 0 && !scfsi[ch][0]) md.write_bits(gi.scalefac[b], slen1);
                for (int b = 6; b < 11; b++)
                    if (slen1 > 0 && !scfsi[ch][1]) md.write_bits(gi.scalefac[b], slen1);
                for (int b = 11; b < 16; b++)
                    if (slen2 > 0 && !scfsi[ch][2]) md.write_bits(gi.scalefac[b], slen2);
                for (int b = 16; b < 21; b++)
                    if (slen2 > 0 && !scfsi[ch][3]) md.write_bits(gi.scalefac[b], slen2);
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
    return enc->output_buf;
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
