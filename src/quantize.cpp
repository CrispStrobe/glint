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

GranuleInfo quantize_granule(const double* mdct_in, int available_bits,
                              int sr_index) {
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

        if (max_energy > 0.0 && active_bands >= 3) {
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
                int max_sf1 = 0, max_sf2 = 0;
                for (int b = 0; b < 11; b++) max_sf1 = std::max(max_sf1, info.scalefac[b]);
                for (int b = 11; b < 21; b++) max_sf2 = std::max(max_sf2, info.scalefac[b]);
                slen1 = 0; while ((1 << slen1) <= max_sf1) slen1++;
                slen2 = 0; while ((1 << slen2) <= max_sf2) slen2++;
                if (slen1 > 4) slen1 = 4;
                if (slen2 > 3) slen2 = 3;
                info.scalefac_compress = encode_scalefac_compress(slen1, slen2);
                info.part2_length = slen1 * 11 + slen2 * 10;
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
    }

    info.regions = huffman_determine_regions(info.ix, sr_index);
    info.part2_3_length = info.part2_length +
                          huffman_count_bits(info.ix, info.regions, sr_index);
    return info;
}

} // namespace glint
