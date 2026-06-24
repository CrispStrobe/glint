// glint - Quantization loop (double-precision input)
// MIT License - Clean-room implementation

#include "quantize.hpp"
#include "psycho.hpp"
#include "tables.hpp"
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace glint {

static double gain_table[256];
static double sf_table[2][16];
static bool tables_init = false;

static void init_quant_tables() {
    if (tables_init) return;
    for (int g = 0; g < 256; g++)
        gain_table[g] = std::pow(2.0, -3.0 * (g - 210.0) / 16.0);
    for (int sf = 0; sf < 16; sf++) {
        // Encoder uses positive exponent to compensate decoder's negative:
        // Decoder: 2^(-0.5*(1+sfs)*sf), Encoder: 2^(+0.75*0.5*(1+sfs)*sf)
        sf_table[0][sf] = std::pow(2.0, 0.75 * sf * 0.5);   // sfs=0
        sf_table[1][sf] = std::pow(2.0, 0.75 * sf * 1.0);   // sfs=1
    }
    tables_init = true;
}

static double fast_pow34(double x) {
    if (x <= 0.0) return 0.0;
    if (x < static_cast<double>(tables::kPow34TableSize - 1)) {
        int idx = static_cast<int>(x);
        double frac = x - idx;
        double a = tables::pow34_table[idx] * (1.0 / 65536.0);
        double b = tables::pow34_table[idx + 1] * (1.0 / 65536.0);
        return a + frac * (b - a);  // linear interpolation
    }
    return std::pow(x, 0.75);
}

// Cache for pre-computed per-coefficient values (constant across binary search)
struct QuantCache {
    double pow34_sf[576]; // pow34(|xr|) * sf_scale — precomputed
    int sign[576];        // +1 or -1
};

static void fill_quant_cache(QuantCache& cache, const double* mdct_in,
                              const int scalefac[21], int scalefac_scale,
                              int preflag, int sr_index) {
    const int* sfb = tables::get_sfb_long_by_unified(sr_index);
    int band = 0;
    for (int i = 0; i < 576; i++) {
        while (band < 21 && i >= sfb[band + 1]) band++;
        int sf = scalefac[band];
        if (preflag && band < 22) sf += tables::preemphasis[band];
        double sfs = (sf > 0 && sf < 16) ? sf_table[scalefac_scale][sf] : 1.0;
        cache.pow34_sf[i] = fast_pow34(std::fabs(mdct_in[i])) * sfs;
        cache.sign[i] = (mdct_in[i] < 0.0) ? -1 : 1;
    }
}

static int quantize_and_count(const double* mdct_in, int* ix,
                               int global_gain, const int scalefac[21],
                               int scalefac_scale, int preflag,
                               int sr_index,
                               HuffRegions* out_regions = nullptr,
                               const QuantCache* cache = nullptr) {
    init_quant_tables();
    double base_step = gain_table[global_gain];

    if (cache) {
        // Fast path: only multiply by base_step (pow34*sf already cached)
        for (int i = 0; i < 576; i++) {
            double qval_d = cache->pow34_sf[i] * base_step + 0.4054;
            int qval = (qval_d >= 8191.0) ? 8191 : static_cast<int>(qval_d);
            ix[i] = cache->sign[i] * qval;
        }
    } else {
        const int* sfb = tables::get_sfb_long_by_unified(sr_index);
        int band = 0;
        for (int i = 0; i < 576; i++) {
            while (band < 21 && i >= sfb[band + 1]) band++;
            int sf = scalefac[band];
            if (preflag && band < 22) sf += tables::preemphasis[band];
            double sf_scale = (sf > 0 && sf < 16) ? sf_table[scalefac_scale][sf] : 1.0;
            double abs_xr = std::fabs(mdct_in[i]);
            double pow34_val = fast_pow34(abs_xr);
            double qval_d = pow34_val * base_step * sf_scale + 0.4054;
            int qval = (qval_d >= 8191.0) ? 8191 : static_cast<int>(qval_d);
            ix[i] = (mdct_in[i] < 0.0) ? -qval : qval;
        }
    }

    HuffRegions regions = huffman_determine_regions(ix, sr_index);
    if (out_regions) *out_regions = regions;
    return huffman_count_bits(ix, regions, sr_index);
}

static int encode_scalefac_compress(int slen1, int slen2) {
    static const int table[16][2] = {
        {0,0},{0,1},{0,2},{0,3},{3,0},{1,1},{1,2},{1,3},
        {2,1},{2,2},{2,3},{3,1},{3,2},{3,3},{4,2},{4,3}
    };
    for (int i = 0; i < 16; i++)
        if (table[i][0] == slen1 && table[i][1] == slen2) return i;
    return 0;
}

// MPEG-2/2.5 scalefac_compress encoding (9 bits).
// For normal (non-intensity) stereo, long blocks:
// sfc < 180: slen[0]=sfc/36, slen[1]=(sfc%36)/6, slen[2]=(sfc%36)%6, slen[3]=0
//   band groups: [6, 5, 5, 5]
// sfc 180..243: slen[0]=(sfc-180)%64/16, slen[1]=(sfc-180)%16/4, slen[2]=(sfc-180)%4, slen[3]=0
//   band groups: [6, 5, 7, 3]
// sfc 244..255: slen[0]=(sfc-244)/3, slen[1]=(sfc-244)%3, slen[2]=0, slen[3]=0
//   band groups: [11, 10, 0, 0]
//
// We use the first range (sfc < 180) for simplicity.
// Given 4 slen values, encode as: sfc = slen[0]*36 + slen[1]*6 + slen[2]
// slen[3] must be 0 for this range.
static int encode_scalefac_compress_m2(int slen0, int slen1, int slen2, int slen3) {
    // If slen3==0 and all fit in range [0,4] for slen0, [0,5] for slen1/2:
    if (slen3 == 0 && slen0 < 5 && slen1 < 6 && slen2 < 6) {
        return slen0 * 36 + slen1 * 6 + slen2;
    }
    // Fallback: use range 244..255 (slen[0]*(3) + slen[1], only 2 groups)
    // This is limited, so try range 180..243 (sfc-180 = s0*16 + s1*4 + s2)
    if (slen3 == 0 && slen0 < 4 && slen1 < 4 && slen2 < 4) {
        return 180 + slen0 * 16 + slen1 * 4 + slen2;
    }
    return 0;  // all zero
}

// Thread-local psycho model for use by quantize_granule
static PsychoModel s_psycho;

GranuleInfo quantize_granule(const double* mdct_in, int available_bits,
                              int sr_index, int quality_mode) {
    // Quality mode 2 (best): apply psychoacoustic masking to zero
    // coefficients below the masking threshold, then quantize with
    // the distortion-controlled outer loop (quality_mode 1).
    if (quality_mode >= 2) {
        double masking_threshold[576];
        s_psycho.compute_masking(mdct_in, masking_threshold, sr_index);

        double mdct_masked[576];
        for (int i = 0; i < 576; i++) {
            if (mdct_in[i] * mdct_in[i] < masking_threshold[i])
                mdct_masked[i] = 0.0;
            else
                mdct_masked[i] = mdct_in[i];
        }

        return quantize_granule(mdct_masked, available_bits, sr_index, 1);
    }

    GranuleInfo info{};
    std::memset(&info, 0, sizeof(info));
    init_quant_tables();

    int slen1 = 0, slen2 = 0;
    info.scalefac_compress = 0;
    info.part2_length = 0;
    int target_bits = available_bits;

    // Compute minimum gain to prevent clipping (ix > 8191).
    // Find the peak MDCT coefficient and determine the gain floor.
    double max_abs = 0.0;
    for (int i = 0; i < 576; i++) {
        double v = std::fabs(mdct_in[i]);
        if (v > max_abs) max_abs = v;
    }
    int min_gain = 0;
    if (max_abs > 0.0) {
        double pow34_peak = fast_pow34(max_abs);
        // Need: pow34_peak * 2^(-3*(g-210)/16) + 0.4054 < 8191
        // pow34_peak * 2^(-3*(g-210)/16) < 8190.6
        // -3*(g-210)/16 < log2(8190.6 / pow34_peak)
        // g > 210 - (16/3) * log2(8190.6 / pow34_peak)
        if (pow34_peak > 0.0) {
            double ratio = 8190.0 / pow34_peak;
            if (ratio > 0.0) {
                double g_min = 210.0 - (16.0 / 3.0) * std::log2(ratio);
                min_gain = static_cast<int>(std::ceil(g_min));
                if (min_gain < 0) min_gain = 0;
            }
        }
    }

    // Pre-compute pow34 * sf_scale for all coefficients (constant across binary search)
    QuantCache cache;
    fill_quant_cache(cache, mdct_in, info.scalefac, info.scalefac_scale,
                     info.preflag, sr_index);

    // Binary search for global_gain.
    // Higher gain = coarser quantization = fewer bits.
    // Lower bound is min_gain to prevent 8191 clipping.
    int lo = min_gain, hi = 255, best_gain = 255;

    for (int iter = 0; iter < 8 && lo <= hi; iter++) {
        int gain = (lo + hi) / 2;
        int bits = quantize_and_count(mdct_in, info.ix, gain, info.scalefac,
                                       info.scalefac_scale, info.preflag, sr_index,
                                       nullptr, &cache);
        if (bits <= target_bits) { hi = gain - 1; best_gain = gain; }
        else { lo = gain + 1; }
    }

    info.global_gain = best_gain;

    // Scalefactor adjustment
    {
        const int* sfb = tables::get_sfb_long_by_unified(sr_index);
        double band_energy[21], max_energy = 0.0;
        for (int band = 0; band < 21; band++) {
            double e = 0.0;
            for (int i = sfb[band]; i < sfb[band+1] && i < 576; i++)
                e += mdct_in[i] * mdct_in[i];
            band_energy[band] = e;
            if (e > max_energy) max_energy = e;
        }

        // Count active bands
        int active_bands = 0;
        if (max_energy > 0.0) {
            for (int band = 0; band < 21; band++)
                if (band_energy[band] / max_energy > 0.01) active_bands++;
        }

        // Helper lambda to compute slen/part2 and re-search gain
        auto recompute_scalefac_encoding = [&]() {
            bool is_mpeg2 = (sr_index >= 3);

            if (is_mpeg2) {
                int max_sf[4] = {};
                for (int b = 0; b < 6; b++) max_sf[0] = std::max(max_sf[0], info.scalefac[b]);
                for (int b = 6; b < 11; b++) max_sf[1] = std::max(max_sf[1], info.scalefac[b]);
                for (int b = 11; b < 16; b++) max_sf[2] = std::max(max_sf[2], info.scalefac[b]);
                for (int b = 16; b < 21; b++) max_sf[3] = std::max(max_sf[3], info.scalefac[b]);
                int sl[4];
                for (int g = 0; g < 4; g++) {
                    sl[g] = 0;
                    while ((1 << sl[g]) <= max_sf[g]) sl[g]++;
                    if (sl[g] > 4) sl[g] = 4;
                }
                info.scalefac_compress = encode_scalefac_compress_m2(
                    sl[0], sl[1], sl[2], sl[3]);
                info.part2_length = sl[0]*6 + sl[1]*5 + sl[2]*5 + sl[3]*5;
            } else {
                int max_sf1 = 0, max_sf2 = 0;
                for (int b = 0; b < 11; b++) max_sf1 = std::max(max_sf1, info.scalefac[b]);
                for (int b = 11; b < 21; b++) max_sf2 = std::max(max_sf2, info.scalefac[b]);
                slen1 = 0; while ((1 << slen1) <= max_sf1) slen1++;
                slen2 = 0; while ((1 << slen2) <= max_sf2) slen2++;
                if (slen1 > 4) slen1 = 4;
                if (slen2 > 3) slen2 = 3;
                info.scalefac_compress = encode_scalefac_compress(slen1, slen2);
                info.part2_length = slen1 * 11 + slen2 * 10;
            }
            target_bits = available_bits - info.part2_length;
            if (target_bits < 0) target_bits = 0;

            lo = min_gain; hi = 255; best_gain = 255;
            for (int iter = 0; iter < 8 && lo <= hi; iter++) {
                int gain = (lo + hi) / 2;
                int bits = quantize_and_count(mdct_in, info.ix, gain, info.scalefac,
                                           info.scalefac_scale, info.preflag, sr_index);
                if (bits <= target_bits) { hi = gain - 1; best_gain = gain; }
                else { lo = gain + 1; }
            }
            info.global_gain = best_gain;
        };

        // Helper lambda to measure per-band distortion
        auto measure_distortion = [&](double* distortion) {
            quantize_and_count(mdct_in, info.ix, best_gain, info.scalefac,
                               info.scalefac_scale, info.preflag, sr_index);
            double decoder_gain = std::pow(2.0, 0.25 * (best_gain - 210));
            for (int band = 0; band < 21; band++) {
                int sf = info.scalefac[band];
                if (info.preflag && band < 22) sf += tables::preemphasis[band];
                double sf_decode = std::pow(2.0, -0.5 * sf * (1 + info.scalefac_scale));
                double noise = 0.0;
                for (int i = sfb[band]; i < sfb[band+1] && i < 576; i++) {
                    double xr_hat = 0.0;
                    if (info.ix[i] != 0) {
                        double abs_ix = std::abs((double)info.ix[i]);
                        xr_hat = std::copysign(
                            std::pow(abs_ix, 4.0/3.0) * decoder_gain * sf_decode,
                            mdct_in[i]);
                    }
                    double err = mdct_in[i] - xr_hat;
                    noise += err * err;
                }
                distortion[band] = noise;
            }
        };

        if (quality_mode >= 1 && max_energy > 0.0) {
            // Distortion-controlled outer loop:
            // Start with SF=0, measure distortion, selectively boost bands
            // that have above-average noise-to-signal ratio.
            for (int outer = 0; outer < 5; outer++) {
                double distortion[21];
                measure_distortion(distortion);

                // Compute average NSR across active bands
                double avg_nsr = 0.0;
                int nsr_count = 0;
                for (int band = 0; band < 21; band++) {
                    if (band_energy[band] > 1e-10) {
                        avg_nsr += distortion[band] / band_energy[band];
                        nsr_count++;
                    }
                }
                if (nsr_count > 0) avg_nsr /= nsr_count;

                // Increase SF for bands with NSR significantly above average
                bool changed = false;
                for (int band = 0; band < 21; band++) {
                    if (band_energy[band] > 1e-10) {
                        double nsr = distortion[band] / band_energy[band];
                        // Boost SF if this band's NSR is > 1.5x the average
                        // and the NSR is above the -10 dB threshold
                        if (nsr > avg_nsr * 1.5 && nsr > 0.1) {
                            if (info.scalefac[band] < 7) {
                                info.scalefac[band]++;
                                changed = true;
                            }
                        }
                    }
                }

                if (!changed) break;

                // Recompute scalefac encoding and re-search gain
                recompute_scalefac_encoding();
            }

            // Check if any SFs are nonzero
            bool any = false;
            for (int b = 0; b < 21; b++) {
                if (info.scalefac[b] > 0) { any = true; break; }
            }
            if (any) {
                recompute_scalefac_encoding();
            }
        } else if (max_energy > 0.0 && active_bands >= 3) {
            // Speed mode: simple energy-based scalefactor assignment
            bool any = false;
            for (int band = 0; band < 21; band++) {
                double ratio = band_energy[band] / max_energy;
                if (ratio > 0.01) {
                    int sf = static_cast<int>(ratio * 4.0 + 0.5);
                    if (sf > 7) sf = 7;
                    if (sf > 0) { info.scalefac[band] = sf; any = true; }
                }
            }
            if (any) {
                recompute_scalefac_encoding();
            }
        }
    }

    // Final quantize to populate ix[] with the chosen gain and scalefactors,
    // and cache the regions to avoid redundant huffman_determine_regions call
    quantize_and_count(mdct_in, info.ix, best_gain, info.scalefac,
                       info.scalefac_scale, info.preflag, sr_index,
                       &info.regions);
    info.part2_3_length = info.part2_length +
                          huffman_count_bits(info.ix, info.regions, sr_index);
    return info;
}

// VBR quality 0-9 maps to target global_gain values:
// 0 (best): gain ~150 (fine quantization, many bits)
// 5 (medium): gain ~186
// 9 (worst): gain ~230 (coarse quantization, few bits)
static const int vbr_target_gain[10] = {
    150, 155, 162, 170, 178, 186, 194, 204, 216, 230
};

GranuleInfo quantize_granule_vbr(const double* mdct_in, int sr_index,
                                  int quality_mode, int vbr_quality) {
    if (quality_mode >= 2) {
        double masking_threshold[576];
        s_psycho.compute_masking(mdct_in, masking_threshold, sr_index);

        double mdct_masked[576];
        for (int i = 0; i < 576; i++) {
            if (mdct_in[i] * mdct_in[i] < masking_threshold[i])
                mdct_masked[i] = 0.0;
            else
                mdct_masked[i] = mdct_in[i];
        }

        return quantize_granule_vbr(mdct_masked, sr_index, 1, vbr_quality);
    }

    GranuleInfo info{};
    std::memset(&info, 0, sizeof(info));
    init_quant_tables();

    if (vbr_quality < 0) vbr_quality = 0;
    if (vbr_quality > 9) vbr_quality = 9;

    int target_gain = vbr_target_gain[vbr_quality];

    // Compute minimum gain to prevent clipping (ix > 8191).
    double max_abs = 0.0;
    for (int i = 0; i < 576; i++) {
        double v = std::fabs(mdct_in[i]);
        if (v > max_abs) max_abs = v;
    }
    int min_gain = 0;
    if (max_abs > 0.0) {
        double pow34_peak = fast_pow34(max_abs);
        if (pow34_peak > 0.0) {
            double ratio = 8190.0 / pow34_peak;
            if (ratio > 0.0) {
                double g_min = 210.0 - (16.0 / 3.0) * std::log2(ratio);
                min_gain = static_cast<int>(std::ceil(g_min));
                if (min_gain < 0) min_gain = 0;
            }
        }
    }

    // Use the target gain, but never go below min_gain (clipping protection)
    int gain = std::max(target_gain, min_gain);
    info.global_gain = gain;

    // Quantize with the chosen gain
    quantize_and_count(mdct_in, info.ix, gain, info.scalefac,
                       info.scalefac_scale, info.preflag, sr_index);

    // Energy-based scalefactor adjustment
    {
        const int* sfb = tables::get_sfb_long_by_unified(sr_index);
        double band_energy[21], max_energy = 0.0;
        for (int band = 0; band < 21; band++) {
            double e = 0.0;
            for (int i = sfb[band]; i < sfb[band+1] && i < 576; i++)
                e += mdct_in[i] * mdct_in[i];
            band_energy[band] = e;
            if (e > max_energy) max_energy = e;
        }

        int active_bands = 0;
        if (max_energy > 0.0) {
            for (int band = 0; band < 21; band++)
                if (band_energy[band] / max_energy > 0.01) active_bands++;
        }

        // Helper lambda to recompute scalefac encoding for VBR
        auto recompute_scalefac_encoding_vbr = [&]() {
            bool is_mpeg2 = (sr_index >= 3);
            if (is_mpeg2) {
                int max_sf[4] = {};
                for (int b = 0; b < 6; b++) max_sf[0] = std::max(max_sf[0], info.scalefac[b]);
                for (int b = 6; b < 11; b++) max_sf[1] = std::max(max_sf[1], info.scalefac[b]);
                for (int b = 11; b < 16; b++) max_sf[2] = std::max(max_sf[2], info.scalefac[b]);
                for (int b = 16; b < 21; b++) max_sf[3] = std::max(max_sf[3], info.scalefac[b]);
                int sl[4];
                for (int g = 0; g < 4; g++) {
                    sl[g] = 0;
                    while ((1 << sl[g]) <= max_sf[g]) sl[g]++;
                    if (sl[g] > 4) sl[g] = 4;
                }
                info.scalefac_compress = encode_scalefac_compress_m2(
                    sl[0], sl[1], sl[2], sl[3]);
                info.part2_length = sl[0]*6 + sl[1]*5 + sl[2]*5 + sl[3]*5;
            } else {
                int max_sf1 = 0, max_sf2 = 0;
                for (int b = 0; b < 11; b++) max_sf1 = std::max(max_sf1, info.scalefac[b]);
                for (int b = 11; b < 21; b++) max_sf2 = std::max(max_sf2, info.scalefac[b]);
                int sl1 = 0; while ((1 << sl1) <= max_sf1) sl1++;
                int sl2 = 0; while ((1 << sl2) <= max_sf2) sl2++;
                if (sl1 > 4) sl1 = 4;
                if (sl2 > 3) sl2 = 3;
                info.scalefac_compress = encode_scalefac_compress(sl1, sl2);
                info.part2_length = sl1 * 11 + sl2 * 10;
            }
        };

        // Helper lambda to measure per-band distortion
        auto measure_distortion = [&](double* distortion) {
            quantize_and_count(mdct_in, info.ix, gain, info.scalefac,
                               info.scalefac_scale, info.preflag, sr_index);
            double decoder_gain = std::pow(2.0, 0.25 * (gain - 210));
            for (int band = 0; band < 21; band++) {
                int sf = info.scalefac[band];
                if (info.preflag && band < 22) sf += tables::preemphasis[band];
                double sf_decode = std::pow(2.0, -0.5 * sf * (1 + info.scalefac_scale));
                double noise = 0.0;
                for (int i = sfb[band]; i < sfb[band+1] && i < 576; i++) {
                    double xr_hat = 0.0;
                    if (info.ix[i] != 0) {
                        double abs_ix = std::abs((double)info.ix[i]);
                        xr_hat = std::copysign(
                            std::pow(abs_ix, 4.0/3.0) * decoder_gain * sf_decode,
                            mdct_in[i]);
                    }
                    double err = mdct_in[i] - xr_hat;
                    noise += err * err;
                }
                distortion[band] = noise;
            }
        };

        if (quality_mode >= 1 && max_energy > 0.0) {
            // Distortion-controlled outer loop for VBR
            for (int outer = 0; outer < 5; outer++) {
                double distortion[21];
                measure_distortion(distortion);

                double avg_nsr = 0.0;
                int nsr_count = 0;
                for (int band = 0; band < 21; band++) {
                    if (band_energy[band] > 1e-10) {
                        avg_nsr += distortion[band] / band_energy[band];
                        nsr_count++;
                    }
                }
                if (nsr_count > 0) avg_nsr /= nsr_count;

                bool changed = false;
                for (int band = 0; band < 21; band++) {
                    if (band_energy[band] > 1e-10) {
                        double nsr = distortion[band] / band_energy[band];
                        if (nsr > avg_nsr * 1.5 && nsr > 0.1) {
                            if (info.scalefac[band] < 7) {
                                info.scalefac[band]++;
                                changed = true;
                            }
                        }
                    }
                }

                if (!changed) break;
                recompute_scalefac_encoding_vbr();
            }

            bool any = false;
            for (int b = 0; b < 21; b++) {
                if (info.scalefac[b] > 0) { any = true; break; }
            }
            if (any) {
                recompute_scalefac_encoding_vbr();
            }
        } else if (max_energy > 0.0 && active_bands >= 3) {
            bool any = false;
            for (int band = 0; band < 21; band++) {
                double ratio = band_energy[band] / max_energy;
                if (ratio > 0.01) {
                    int sf = static_cast<int>(ratio * 4.0 + 0.5);
                    if (sf > 7) sf = 7;
                    if (sf > 0) { info.scalefac[band] = sf; any = true; }
                }
            }
            if (any) {
                recompute_scalefac_encoding_vbr();
                quantize_and_count(mdct_in, info.ix, gain, info.scalefac,
                                   info.scalefac_scale, info.preflag, sr_index);
            }
        }
    }

    // Final quantize and cache regions
    quantize_and_count(mdct_in, info.ix, gain, info.scalefac,
                       info.scalefac_scale, info.preflag, sr_index,
                       &info.regions);
    info.part2_3_length = info.part2_length +
                          huffman_count_bits(info.ix, info.regions, sr_index);
    return info;
}

} // namespace glint
