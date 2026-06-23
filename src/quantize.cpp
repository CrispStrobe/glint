// glint - Quantization loop (double-precision input)
// MIT License - Clean-room implementation

#include "quantize.hpp"
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

static int quantize_and_count(const double* mdct_in, int* ix,
                               int global_gain, const int scalefac[21],
                               int scalefac_scale, int preflag,
                               int sr_index) {
    init_quant_tables();
    const int* sfb = tables::get_sfb_long_by_unified(sr_index);
    double base_step = gain_table[global_gain];

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

    HuffRegions regions = huffman_determine_regions(ix, sr_index);
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

GranuleInfo quantize_granule(const double* mdct_in, int available_bits,
                              int sr_index, int quality_mode) {
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

    // Binary search for global_gain.
    // Higher gain = coarser quantization = fewer bits.
    // Lower bound is min_gain to prevent 8191 clipping.
    int lo = min_gain, hi = 255, best_gain = 255;

    for (int iter = 0; iter < 20 && lo <= hi; iter++) {
        int gain = (lo + hi) / 2;
        int bits = quantize_and_count(mdct_in, info.ix, gain, info.scalefac,
                                       info.scalefac_scale, info.preflag, sr_index);
        if (bits <= target_bits) { hi = gain - 1; best_gain = gain; }
        else { lo = gain + 1; }
    }

    info.global_gain = best_gain;
    quantize_and_count(mdct_in, info.ix, best_gain, info.scalefac,
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

        // Count active bands - only apply scalefactors when enough spectral
        // diversity exists to benefit from noise shaping
        int active_bands = 0;
        if (max_energy > 0.0) {
            for (int band = 0; band < 21; band++)
                if (band_energy[band] / max_energy > 0.01) active_bands++;
        }

        bool any = false;
        if (quality_mode == 1 && max_energy > 0.0) {
            // Spreading function: each band masks neighbors at ~8 dB/band distance
            double masking[21] = {};
            for (int i = 0; i < 21; i++) {
                for (int j = 0; j < 21; j++) {
                    double spread = std::pow(10.0, -std::abs(i - j) * 0.8);
                    masking[i] += band_energy[j] * spread;
                }
            }

            // Scalefactor = inverse of signal-to-mask ratio
            for (int band = 0; band < 21; band++) {
                if (masking[band] > 0 && band_energy[band] > 0) {
                    double smr = band_energy[band] / masking[band];
                    int sf = 7 - static_cast<int>(std::log2(std::max(smr, 0.001)) * 1.5);
                    if (sf < 0) sf = 0;
                    if (sf > 7) sf = 7;
                    info.scalefac[band] = sf;
                    if (sf > 0) any = true;
                }
            }
        } else if (max_energy > 0.0 && active_bands >= 3) {
            for (int band = 0; band < 21; band++) {
                double ratio = band_energy[band] / max_energy;
                if (ratio > 0.01) {
                    int sf = static_cast<int>(ratio * 4.0 + 0.5);
                    if (sf > 7) sf = 7;
                    if (sf > 0) { info.scalefac[band] = sf; any = true; }
                }
            }
        }

        if (any) {
            bool is_mpeg2 = (sr_index >= 3);

            if (is_mpeg2) {
                // MPEG-2/2.5: 4 band groups [6, 5, 5, 5] = 21 bands
                // Group 0: bands 0-5, Group 1: bands 6-10,
                // Group 2: bands 11-15, Group 3: bands 16-20
                int max_sf[4] = {};
                for (int b = 0; b < 6; b++) max_sf[0] = std::max(max_sf[0], info.scalefac[b]);
                for (int b = 6; b < 11; b++) max_sf[1] = std::max(max_sf[1], info.scalefac[b]);
                for (int b = 11; b < 16; b++) max_sf[2] = std::max(max_sf[2], info.scalefac[b]);
                for (int b = 16; b < 21; b++) max_sf[3] = std::max(max_sf[3], info.scalefac[b]);
                int slen[4];
                for (int g = 0; g < 4; g++) {
                    slen[g] = 0;
                    while ((1 << slen[g]) <= max_sf[g]) slen[g]++;
                    if (slen[g] > 4) slen[g] = 4;
                }
                info.scalefac_compress = encode_scalefac_compress_m2(
                    slen[0], slen[1], slen[2], slen[3]);
                info.part2_length = slen[0]*6 + slen[1]*5 + slen[2]*5 + slen[3]*5;
            } else {
                // MPEG-1: 2 band groups [11, 10] = 21 bands
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
            for (int iter = 0; iter < 16 && lo <= hi; iter++) {
                int gain = (lo + hi) / 2;
                int bits = quantize_and_count(mdct_in, info.ix, gain, info.scalefac,
                                           info.scalefac_scale, info.preflag, sr_index);
                if (bits <= target_bits) { hi = gain; best_gain = gain; }
                else { lo = gain + 1; }
            }
            info.global_gain = best_gain;
            quantize_and_count(mdct_in, info.ix, best_gain, info.scalefac,
                               info.scalefac_scale, info.preflag, sr_index);
        }
    }

    info.regions = huffman_determine_regions(info.ix, sr_index);
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

    // Energy-based scalefactor adjustment (same logic as CBR)
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

        bool any = false;
        if (quality_mode == 1 && max_energy > 0.0) {
            double masking[21] = {};
            for (int i = 0; i < 21; i++) {
                for (int j = 0; j < 21; j++) {
                    double spread = std::pow(10.0, -std::abs(i - j) * 0.8);
                    masking[i] += band_energy[j] * spread;
                }
            }
            for (int band = 0; band < 21; band++) {
                if (masking[band] > 0 && band_energy[band] > 0) {
                    double smr = band_energy[band] / masking[band];
                    int sf = 7 - static_cast<int>(std::log2(std::max(smr, 0.001)) * 1.5);
                    if (sf < 0) sf = 0;
                    if (sf > 7) sf = 7;
                    info.scalefac[band] = sf;
                    if (sf > 0) any = true;
                }
            }
        } else if (max_energy > 0.0 && active_bands >= 3) {
            for (int band = 0; band < 21; band++) {
                double ratio = band_energy[band] / max_energy;
                if (ratio > 0.01) {
                    int sf = static_cast<int>(ratio * 4.0 + 0.5);
                    if (sf > 7) sf = 7;
                    if (sf > 0) { info.scalefac[band] = sf; any = true; }
                }
            }
        }

        if (any) {
            bool is_mpeg2 = (sr_index >= 3);
            if (is_mpeg2) {
                int max_sf[4] = {};
                for (int b = 0; b < 6; b++) max_sf[0] = std::max(max_sf[0], info.scalefac[b]);
                for (int b = 6; b < 11; b++) max_sf[1] = std::max(max_sf[1], info.scalefac[b]);
                for (int b = 11; b < 16; b++) max_sf[2] = std::max(max_sf[2], info.scalefac[b]);
                for (int b = 16; b < 21; b++) max_sf[3] = std::max(max_sf[3], info.scalefac[b]);
                int slen[4];
                for (int g = 0; g < 4; g++) {
                    slen[g] = 0;
                    while ((1 << slen[g]) <= max_sf[g]) slen[g]++;
                    if (slen[g] > 4) slen[g] = 4;
                }
                info.scalefac_compress = encode_scalefac_compress_m2(
                    slen[0], slen[1], slen[2], slen[3]);
                info.part2_length = slen[0]*6 + slen[1]*5 + slen[2]*5 + slen[3]*5;
            } else {
                int max_sf1 = 0, max_sf2 = 0;
                for (int b = 0; b < 11; b++) max_sf1 = std::max(max_sf1, info.scalefac[b]);
                for (int b = 11; b < 21; b++) max_sf2 = std::max(max_sf2, info.scalefac[b]);
                int slen1 = 0; while ((1 << slen1) <= max_sf1) slen1++;
                int slen2 = 0; while ((1 << slen2) <= max_sf2) slen2++;
                if (slen1 > 4) slen1 = 4;
                if (slen2 > 3) slen2 = 3;
                info.scalefac_compress = encode_scalefac_compress(slen1, slen2);
                info.part2_length = slen1 * 11 + slen2 * 10;
            }

            // Re-quantize with scalefactors applied
            quantize_and_count(mdct_in, info.ix, gain, info.scalefac,
                               info.scalefac_scale, info.preflag, sr_index);
        }
    }

    info.regions = huffman_determine_regions(info.ix, sr_index);
    info.part2_3_length = info.part2_length +
                          huffman_count_bits(info.ix, info.regions, sr_index);
    return info;
}

} // namespace glint
