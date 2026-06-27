// glint - Quantization loop (double-precision input)
// MIT License - Clean-room implementation

#include "quantize.hpp"
#include "psycho.hpp"
#include "tables.hpp"
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <cstdio>

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
        return tables::pow34_table[idx] + frac * (tables::pow34_table[idx + 1] - tables::pow34_table[idx]);
    }
    return std::pow(x, 0.75);
}

// Cache for pre-computed per-coefficient values (constant across binary search)
struct QuantCache {
    double pow34_sf[576]; // pow34(|xr|) * sf_scale - precomputed
    int8_t sign[576];     // +1 or -1
};

static int find_count1_start(const int16_t* ix, int rzero) {
    int count1_start = (rzero + 3) & ~3;
    if (count1_start > rzero) count1_start = rzero;

    while (count1_start >= 4) {
        bool all_small = true;
        for (int i = count1_start - 4; i < count1_start; i++) {
            if (i < 576 && std::abs(ix[i]) > 1) {
                all_small = false;
                break;
            }
        }
        if (!all_small) break;
        count1_start -= 4;
    }

    if (count1_start & 1) count1_start++;
    return count1_start;
}

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

static int quantize_and_count(const double* mdct_in, int16_t* ix,
                               int global_gain, const int scalefac[21],
                               int scalefac_scale, int preflag,
                               int sr_index,
                               HuffRegions* out_regions = nullptr,
                               const QuantCache* cache = nullptr,
                               bool short_block = false,
                               int bit_limit = -1) {
    init_quant_tables();
    double base_step = gain_table[global_gain];

    // Fused quantize + region detection in a single pass
    int rzero = 0;          // last nonzero index + 1

    if (cache) {
        for (int i = 0; i < 576; i++) {
            double qval_d = cache->pow34_sf[i] * base_step + 0.4054;
            int qval = (qval_d >= 8191.0) ? 8191 : static_cast<int>(qval_d);
            ix[i] = cache->sign[i] * qval;
        }
        // Scan backward for rzero (avoids branch in hot loop)
        rzero = 576;
        while (rzero > 0 && ix[rzero - 1] == 0) rzero--;
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
        rzero = 576;
        while (rzero > 0 && ix[rzero - 1] == 0) rzero--;
    }

    // Short blocks use a different region layout; the gain search must count
    // bits with the same layout the encoder will actually use.
    int count1_start = find_count1_start(ix, rzero);
    HuffRegions regions = short_block
        ? huffman_determine_regions_short_from_bounds(ix, sr_index, rzero,
                                                      count1_start)
        : huffman_determine_regions_from_bounds(ix, sr_index, rzero,
                                                count1_start);
    if (out_regions) *out_regions = regions;
    return bit_limit >= 0 ? huffman_count_bits_limited(ix, regions, sr_index,
                                                       bit_limit)
                          : huffman_count_bits(ix, regions, sr_index);
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

// Compute headroom-based scalefactors using Vorbis/Opus/FLAC psychoacoustic
// masking model. Assigns SF inversely proportional to masking headroom:
// loud bands (high SMR) get sf=0 (noise masked by signal), bands near
// masking threshold get higher SF (need precision), bands below threshold
// get sf=0 (inaudible, save bits).
static bool compute_headroom_scalefactors(int scalefac[21],
                                           const double band_energy[21],
                                           int /* active_bands */) {
    // Compute per-band masking threshold via simplified spreading function
    double band_mask[21];
    for (int band = 0; band < 21; band++) {
        band_mask[band] = 0;
        for (int other = 0; other < 21; other++) {
            if (other == band) continue;  // exclude self-masking
            double spread;
            if (other > band)
                spread = std::pow(10.0, -0.8 * (other - band));  // 8 dB/band upward
            else
                spread = std::pow(10.0, -0.4 * (band - other));  // 4 dB/band downward
            band_mask[band] += band_energy[other] * spread;
        }
        // ATH floor, scaled to MDCT energy domain (MDCT divides by 288)
        double ath_linear = std::pow(10.0, (tables::ath_cb[std::min(band, 24)] - 96.0) / 10.0);
        ath_linear /= (288.0 * 288.0);
        // Use 0.1 (-10 dB) masking offset for inter-band masking
        band_mask[band] = std::max(band_mask[band] * 0.1, ath_linear);
    }

    // SMR (signal-to-mask ratio) per band, assign SF
    // Bands with energy well above their mask have lots of headroom and
    // can tolerate coarse quantization. Bands near the mask need precision.
    bool any = false;
    for (int band = 0; band < 21; band++) {
        double smr = band_energy[band] / band_mask[band];
        double headroom_db = 10.0 * std::log10(std::max(smr, 1e-10));
        // Map headroom to SF with wider range for better differentiation:
        // headroom <= 0 dB  -> sf = 0 (below mask, inaudible)
        // headroom  0-30 dB -> sf = 7..0 (linear mapping)
        // headroom > 30 dB  -> sf = 0 (far above mask, self-masked)
        int sf;
        if (headroom_db <= 0.0)
            sf = 0;
        else if (headroom_db > 30.0)
            sf = 0;
        else
            sf = static_cast<int>((30.0 - headroom_db) * 7.0 / 30.0);
        scalefac[band] = sf;
        if (sf > 0) any = true;
    }
    return any;
}

// Decoder-reconstruction MSE for a quantized granule vs the original MDCT.
// Used to pick the per-granule input scale ("factor") that best preserves the
// spectrum through the quantizer dead-zone.
static double granule_mse(const GranuleInfo& gi, const double* mdct_in,
                          int sr_index, int quality_mode) {
    const int* sfb = tables::get_sfb_long_by_unified(sr_index);
    double decoder_gain = std::pow(2.0, 0.25 * (gi.global_gain - 210));
    double noise = 0.0;
    double src_band[21] = {};
    double rec_band[21] = {};
    int b = 0;
    for (int i = 0; i < 576; i++) {
        while (b < 20 && i >= sfb[b + 1]) b++;
        int sf = gi.scalefac[b];
        if (gi.preflag && b < 22) sf += tables::preemphasis[b];
        double sf_d = std::pow(2.0, -0.5 * sf * (1 + gi.scalefac_scale));
        double xr_hat = 0.0;
        if (gi.ix[i] != 0) {
            double a = std::abs(static_cast<double>(gi.ix[i]));
            xr_hat = std::copysign(std::pow(a, 4.0/3.0) * decoder_gain * sf_d,
                                   mdct_in[i]);
        }
        double err = mdct_in[i] - xr_hat;
        noise += err * err;
        src_band[b] += mdct_in[i] * mdct_in[i];
        rec_band[b] += xr_hat * xr_hat;
    }

    if (quality_mode <= 0) return noise;

    double total = 0.0;
    for (int band = 0; band < 21; band++)
        total += src_band[band];
    if (total <= 0.0) return noise;

    double envelope_penalty = 0.0;
    for (int band = 0; band < 21; band++) {
        if (src_band[band] < total * 1e-4) continue;
        double retention = rec_band[band] / (src_band[band] + 1e-18);
        if (retention >= 0.9) continue;
        double loss = std::log(0.9 / std::max(retention, 1e-6));
        double band_weight = (band >= 12) ? 3.0 : 0.75;
        envelope_penalty += band_weight * src_band[band] * loss * loss;
    }
    return noise + 0.15 * envelope_penalty;
}

// Base quantizer: gain search to the bit budget + energy-based scalefactors.
// Operates on already-scaled input; the public quantize_granule wraps this in
// the per-granule scale search.
static GranuleInfo quantize_base(const double* mdct_in, int available_bits,
                                 int sr_index, bool short_block);

// Per-granule scale search (all quality tiers).
//
// The quantizer is is = int(|xr|^0.75 * step + 0.4054); coefficients whose
// scaled magnitude is below the ~0.6 dead-zone round to zero. Small (mostly
// high-frequency) coefficients are lost, which both dulls the signal and drops
// its level. A single global gain cannot fix this because the right pre-scale
// depends on each granule's spectral shape — so we search input scale factors
// and keep the one whose decoder reconstruction best matches the original (min
// MSE). This replaces the older fixed 288/194 "gain correction" and the
// (measurably no-op) headroom/pruning passes. Wider search = higher quality and
// more CPU: speed=2 factors, normal=6, best=12 + a fine refinement.
GranuleInfo quantize_granule(const double* mdct_in, int available_bits,
                              int sr_index, int quality_mode, bool short_block) {
    init_quant_tables();
    static const double kSpeed[]  = { 1.7, 2.4 };
    static const double kNormal[] = { 1.3, 1.7, 2.1, 2.6, 3.2, 4.0 };
    static const double kBest[]   = {
        1.0, 1.2, 1.4, 1.6, 1.8, 2.0, 2.2, 2.5, 2.8, 3.2, 3.6, 4.2 };
    const double* factors; int nf;
    if (quality_mode >= 2)      { factors = kBest;   nf = 12; }
    else if (quality_mode == 1) { factors = kNormal; nf = 6; }
    else                        { factors = kSpeed;  nf = 2; }

    GranuleInfo best_result{};
    double best_mse = 1e30;
    double best_factor = 1.0;
    double scaled[576];
    for (int fi = 0; fi < nf; fi++) {
        double f = factors[fi];
        for (int i = 0; i < 576; i++) scaled[i] = mdct_in[i] * f;
        GranuleInfo gi = quantize_base(scaled, available_bits, sr_index, short_block);
        double d = granule_mse(gi, mdct_in, sr_index, quality_mode);
        if (d < best_mse) { best_mse = d; best_result = gi; best_factor = f; }
    }
    if (quality_mode >= 2) {
        for (int step = -2; step <= 2; step++) {
            if (step == 0) continue;
            double f = best_factor + step * 0.12;
            if (f < 0.5) continue;
            for (int i = 0; i < 576; i++) scaled[i] = mdct_in[i] * f;
            GranuleInfo gi = quantize_base(scaled, available_bits, sr_index, short_block);
            double d = granule_mse(gi, mdct_in, sr_index, quality_mode);
            if (d < best_mse) { best_mse = d; best_result = gi; }
        }
    }
    return best_result;
}

static GranuleInfo quantize_base(const double* mdct_in, int available_bits,
                                 int sr_index, bool short_block) {
    GranuleInfo info{};
    std::memset(&info, 0, sizeof(info));
    init_quant_tables();

    int slen1 = 0, slen2 = 0;
    info.scalefac_compress = 0;
    info.part2_length = 0;
    int target_bits = available_bits;

    // Compute minimum gain to prevent clipping (ix > 8191).
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

    // Tighten binary search upper bound: gain where the peak quantizes to ~1.
    // pow34_peak * gain_table[g] + 0.4054 < 1.0 → g > 210 - (16/3)*log2(0.6/pow34_peak)
    int max_gain = 255;
    if (max_abs > 0.0) {
        double pow34_peak = fast_pow34(max_abs);
        if (pow34_peak > 0.0) {
            double g_max = 210.0 - (16.0 / 3.0) * std::log2(0.6 / pow34_peak);
            int est = static_cast<int>(g_max) + 2;
            if (est < max_gain && est > min_gain) max_gain = est;
        }
    }

    // Binary search for global_gain.
    int lo = min_gain, hi = max_gain, best_gain = max_gain;

    for (int iter = 0; iter < 8 && lo <= hi; iter++) {
        int gain = (lo + hi) / 2;
        int bits = quantize_and_count(mdct_in, info.ix, gain, info.scalefac,
                                       info.scalefac_scale, info.preflag, sr_index,
                                       nullptr, &cache, short_block, target_bits);
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

            QuantCache sf_cache;
            fill_quant_cache(sf_cache, mdct_in, info.scalefac,
                             info.scalefac_scale, info.preflag, sr_index);

            lo = min_gain; hi = max_gain; best_gain = max_gain;
            for (int iter = 0; iter < 8 && lo <= hi; iter++) {
                int gain = (lo + hi) / 2;
                int bits = quantize_and_count(mdct_in, info.ix, gain, info.scalefac,
                                           info.scalefac_scale, info.preflag, sr_index,
                                           nullptr, &sf_cache, short_block,
                                           target_bits);
                if (bits <= target_bits) { hi = gain - 1; best_gain = gain; }
                else { lo = gain + 1; }
            }
            info.global_gain = best_gain;
        };

        if (max_energy > 0.0 && active_bands >= 3) {
            // Energy-based scalefactor assignment: give a little extra precision
            // to higher-energy bands. Modes 1/2 get their per-granule fidelity
            // from the scale search that wraps this base quantizer.
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
    QuantCache final_cache;
    fill_quant_cache(final_cache, mdct_in, info.scalefac, info.scalefac_scale,
                     info.preflag, sr_index);
    int huff_bits = quantize_and_count(mdct_in, info.ix, best_gain,
                                       info.scalefac, info.scalefac_scale,
                                       info.preflag, sr_index, &info.regions,
                                       &final_cache, short_block);
    info.part2_3_length = info.part2_length + huff_bits;

    // Budget guarantee: part2_3_length must fit both the per-granule bit
    // budget and the 12-bit side-info field. The gain search above counts
    // bits with the correct (long/short) region layout, so this rarely
    // triggers, but it is a hard safety net: coarsen the quantization
    // (raise global_gain) until the granule fits. Without it, an overflowing
    // part2_3_length wraps the 12-bit field and desyncs the decoder.
    {
        int limit = available_bits;
        if (limit > 4095) limit = 4095;  // 12-bit part2_3_length field
        while (info.part2_3_length > limit && info.global_gain < 255) {
            info.global_gain++;
            huff_bits = quantize_and_count(mdct_in, info.ix, info.global_gain,
                                           info.scalefac, info.scalefac_scale,
                                           info.preflag, sr_index, &info.regions,
                                           &final_cache, short_block);
            info.part2_3_length = info.part2_length + huff_bits;
        }
    }
    return info;
}

// VBR quality 0-9 maps to target global_gain values:
// 0 (best): gain ~150 (fine quantization, many bits)
// 5 (medium): gain ~186
// 9 (worst): gain ~230 (coarse quantization, few bits)
static const int vbr_target_gain[10] = {
    150, 155, 162, 170, 178, 186, 194, 204, 216, 230
};

GranuleInfo quantize_granule_vbr(const double* mdct_in, int available_bits,
                                  int sr_index, int quality_mode, int vbr_quality,
                                  bool short_block) {
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

        return quantize_granule_vbr(mdct_masked, available_bits, sr_index, 1,
                                    vbr_quality, short_block);
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

        if (quality_mode >= 1 && max_energy > 0.0 && active_bands >= 3) {
            // Headroom-based scalefactor assignment (Vorbis/Opus/FLAC insight)
            bool any = compute_headroom_scalefactors(info.scalefac, band_energy,
                                                      active_bands);
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
            }
        }
    }

    // Final quantize and cache regions (using the correct long/short layout)
    QuantCache final_cache;
    fill_quant_cache(final_cache, mdct_in, info.scalefac, info.scalefac_scale,
                     info.preflag, sr_index);
    int huff_bits = quantize_and_count(mdct_in, info.ix, gain, info.scalefac,
                                       info.scalefac_scale, info.preflag,
                                       sr_index, &info.regions, &final_cache,
                                       short_block);
    info.part2_3_length = info.part2_length + huff_bits;

    // Budget guarantee: with the reservoir disabled, each granule must fit its
    // share of the frame, and part2_3_length must also fit the 12-bit side-info
    // field (max 4095). A fine target gain on a loud granule can exceed either,
    // so coarsen global_gain until it fits. available_bits <= 0 means "no frame
    // budget given" — fall back to just the field limit.
    int limit = (available_bits > 0) ? available_bits : 4095;
    if (limit > 4095) limit = 4095;
    while (info.part2_3_length > limit && gain < 255) {
        gain++;
        info.global_gain = gain;
        huff_bits = quantize_and_count(mdct_in, info.ix, gain, info.scalefac,
                                       info.scalefac_scale, info.preflag,
                                       sr_index, &info.regions, &final_cache,
                                       short_block);
        info.part2_3_length = info.part2_length + huff_bits;
    }
    return info;
}

} // namespace glint
