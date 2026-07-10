// CELT frame encoder — RFC 6716 section 4.3 (encoder side)
// MIT License - Clean-room implementation

#include "opus_celt_encoder.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "opus_celt_enc_bands.hpp"
#include "opus_celt_pitch.hpp"
#include "opus_celt_enc_energy.hpp"
#include "opus_celt_rate.hpp"
#include "opus_celt_tables.hpp"

namespace glint {
namespace opus {

namespace {
using celt::kEBands;
using celt::kNbEBands;
constexpr float kPreemphCoef = 0.85f;
constexpr int kSpreadNormal = 2;
constexpr int kCombFilterMinPeriodEnc = 15;

// tf_select_table (RFC 6716): per-LM effective tf resolution for
// [4*isTransient + 2*tf_select + tf_res]. Shared by tf_analysis,
// tf_encode and the post-encode remap that quant_all_bands consumes.
const signed char kTfSelectTable[4][8] = {
    { 0, -1, 0, -1, 0, -1, 0, -1 },
    { 0, -1, 0, -2, 1, 0, 1, -1 },
    { 0, -2, 0, -3, 2, 0, 1, -1 },
    { 0, -2, 0, -3, 3, 0, 1, -1 },
};

// Forward/backward-masking transient detector (reference float path).
// Pure encoder policy: the decision changes quality, never validity.
// Also produces tf_estimate (transient strength, feeds the tf_analysis
// bias) and tf_chan (the channel that tripped the metric).
int transient_analysis(const double* in, int len, int C,
                       double* tf_estimate, int* tf_chan) {
    // 6*64/x lookup, reference-trained.
    static const uint8_t kInvTable[128] = {
        255, 255, 156, 110, 86, 70, 59, 51, 45, 40, 37, 33, 31, 28, 26, 25,
        23,  22,  21,  20,  19, 18, 17, 16, 16, 15, 15, 14, 13, 13, 12, 12,
        12,  12,  11,  11,  11, 10, 10, 10, 9,  9,  9,  9,  9,  9,  8,  8,
        8,   8,   8,   7,   7,  7,  7,  7,  7,  6,  6,  6,  6,  6,  6,  6,
        6,   6,   6,   6,   6,  6,  6,  6,  6,  5,  5,  5,  5,  5,  5,  5,
        5,   5,   5,   5,   5,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  4,
        4,   4,   4,   4,   4,  4,  4,  4,  4,  4,  4,  4,  4,  4,  3,  3,
        3,   3,   3,   3,   3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  2,
    };
    static thread_local double tmp[2 * (960 + 120)];
    int len2 = len / 2;
    int32_t mask_metric = 0;
    for (int c = 0; c < C; c++) {
        // High-pass (1 - 2z^-1 + z^-2)/(1 - z^-1 + .5z^-2).
        double mem0 = 0, mem1 = 0;
        for (int i = 0; i < len; i++) {
            double x = in[c * (960 + 120) + i];
            double y = mem0 + x;
            mem0 = mem1 + y - 2 * x;
            mem1 = x - 0.5 * y;
            tmp[i] = y;
        }
        for (int i = 0; i < 12; i++) tmp[i] = 0;

        // Forward pass: post-echo threshold (6.7 dB/ms decay).
        double mean = 0, mem = 0;
        const double fwd = 0.0625;
        for (int i = 0; i < len2; i++) {
            double x2 = tmp[2 * i] * tmp[2 * i] +
                        tmp[2 * i + 1] * tmp[2 * i + 1];
            mean += x2;
            mem = x2 + (1.0 - fwd) * mem;
            tmp[i] = fwd * mem;
        }
        // Backward pass: pre-echo threshold (13.9 dB/ms).
        mem = 0;
        double max_e = 0;
        for (int i = len2 - 1; i >= 0; i--) {
            mem = tmp[i] + 0.875 * mem;
            tmp[i] = 0.125 * mem;
            if (tmp[i] > max_e) max_e = tmp[i];
        }
        // Bitrate-normalized temporal noise-to-mask ratio: harmonic mean
        // of the threshold vs the (geometric-mean) frame energy.
        mean = std::sqrt(mean * max_e * 0.5 * len2);
        double norm = len2 / (1e-15 + mean);
        int32_t unmask = 0;
        for (int i = 12; i < len2 - 5; i += 4) {
            double v = std::floor(64.0 * norm * (tmp[i] + 1e-15));
            int id = v < 0 ? 0 : (v > 127 ? 127 : static_cast<int>(v));
            unmask += kInvTable[id];
        }
        unmask = 64 * unmask * 4 / (6 * (len2 - 17));
        if (unmask > mask_metric) {
            *tf_chan = c;
            mask_metric = unmask;
        }
    }
    double tf_max =
        std::max(0.0, std::sqrt(27.0 * mask_metric) - 42.0);
    *tf_estimate = std::sqrt(
        std::max(0.0, 0.0069 * std::min(163.0, tf_max) - 0.139));
    return mask_metric > 200;
}

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }

// Local float Haar butterfly (the enc_bands copy is TU-private).
void haar1f(float* x, int n0, int stride) {
    n0 >>= 1;
    for (int i = 0; i < stride; i++) {
        for (int j = 0; j < n0; j++) {
            float tmp1 = 0.70710678f * x[stride * 2 * j + i];
            float tmp2 = 0.70710678f * x[stride * (2 * j + 1) + i];
            x[stride * 2 * j + i] = tmp1 + tmp2;
            x[stride * (2 * j + 1) + i] = tmp1 - tmp2;
        }
    }
}

// L1 norm with a bias toward frequency resolution when in doubt.
double l1_metric(const float* tmp, int n, int lm, double bias) {
    double l1 = 0;
    for (int i = 0; i < n; i++) l1 += std::fabs(tmp[i]);
    return l1 + lm * bias * l1;
}

// Per-band time-frequency resolution analysis (reference tf_analysis):
// L1-sparsity of each band under successive Haar merges/splits picks a
// per-band metric; a Viterbi pass with switching cost lambda smooths
// the change bits; tf_select picks the better of the two table halves.
// Policy-only: any tf_res is a valid stream.
int tf_analysis(int len, int is_transient, int* tf_res, int lambda,
                const float* X, int n0, int lm, double tf_estimate,
                int tf_chan, const int* importance) {
    // 176 = widest band (22) << max LM (3).
    float tmp[176], tmp_1[176];
    int metric[kNbEBands], path0[kNbEBands], path1[kNbEBands];
    double bias = 0.04 * std::max(-0.25, 0.5 - tf_estimate);

    for (int i = 0; i < len; i++) {
        int N = (kEBands[i + 1] - kEBands[i]) << lm;
        // Band too narrow to be split down to LM=-1.
        int narrow = (kEBands[i + 1] - kEBands[i]) == 1;
        std::memcpy(tmp, &X[tf_chan * n0 + (kEBands[i] << lm)],
                    N * sizeof(float));
        double L1 = l1_metric(tmp, N, is_transient ? lm : 0, bias);
        double best_L1 = L1;
        int best_level = 0;
        // The -1 case (extra merge) for transients.
        if (is_transient && !narrow) {
            std::memcpy(tmp_1, tmp, N * sizeof(float));
            haar1f(tmp_1, N >> lm, 1 << lm);
            L1 = l1_metric(tmp_1, N, lm + 1, bias);
            if (L1 < best_L1) {
                best_L1 = L1;
                best_level = -1;
            }
        }
        for (int k = 0; k < lm + !(is_transient || narrow); k++) {
            int B = is_transient ? lm - k - 1 : k + 1;
            haar1f(tmp, N >> k, 1 << k);
            L1 = l1_metric(tmp, N, B, bias);
            if (L1 < best_L1) {
                best_L1 = L1;
                best_level = k + 1;
            }
        }
        // Q1 metric so narrow bands can sit on the half-way point.
        metric[i] = is_transient ? 2 * best_level : -2 * best_level;
        if (narrow && (metric[i] == 0 || metric[i] == -2 * lm))
            metric[i] -= 1;
    }

    // Try both tf_select settings.
    int selcost[2];
    for (int sel = 0; sel < 2; sel++) {
        int cost0 =
            importance[0] *
            std::abs(metric[0] -
                     2 * kTfSelectTable[lm][4 * is_transient + 2 * sel]);
        int cost1 =
            importance[0] *
                std::abs(
                    metric[0] -
                    2 * kTfSelectTable[lm][4 * is_transient + 2 * sel + 1]) +
            (is_transient ? 0 : lambda);
        for (int i = 1; i < len; i++) {
            int curr0 = imin(cost0, cost1 + lambda);
            int curr1 = imin(cost0 + lambda, cost1);
            cost0 = curr0 +
                    importance[i] *
                        std::abs(metric[i] -
                                 2 * kTfSelectTable[lm]
                                                   [4 * is_transient +
                                                    2 * sel]);
            cost1 = curr1 +
                    importance[i] *
                        std::abs(metric[i] -
                                 2 * kTfSelectTable[lm]
                                                   [4 * is_transient +
                                                    2 * sel + 1]);
        }
        selcost[sel] = imin(cost0, cost1);
    }
    // Conservative: tf_select=1 only for transients (reference policy).
    int tf_select = (selcost[1] < selcost[0] && is_transient) ? 1 : 0;

    // Viterbi forward pass.
    int cost0 =
        importance[0] *
        std::abs(metric[0] -
                 2 * kTfSelectTable[lm][4 * is_transient + 2 * tf_select]);
    int cost1 =
        importance[0] *
            std::abs(metric[0] -
                     2 * kTfSelectTable[lm]
                                       [4 * is_transient + 2 * tf_select +
                                        1]) +
        (is_transient ? 0 : lambda);
    for (int i = 1; i < len; i++) {
        int from0 = cost0, from1 = cost1 + lambda;
        int curr0;
        if (from0 < from1) {
            curr0 = from0;
            path0[i] = 0;
        } else {
            curr0 = from1;
            path0[i] = 1;
        }
        from0 = cost0 + lambda;
        from1 = cost1;
        int curr1;
        if (from0 < from1) {
            curr1 = from0;
            path1[i] = 0;
        } else {
            curr1 = from1;
            path1[i] = 1;
        }
        cost0 = curr0 +
                importance[i] *
                    std::abs(metric[i] -
                             2 * kTfSelectTable[lm][4 * is_transient +
                                                    2 * tf_select]);
        cost1 = curr1 +
                importance[i] *
                    std::abs(metric[i] -
                             2 * kTfSelectTable[lm][4 * is_transient +
                                                    2 * tf_select + 1]);
    }
    tf_res[len - 1] = cost0 < cost1 ? 0 : 1;
    // Backward pass.
    for (int i = len - 2; i >= 0; i--)
        tf_res[i] = tf_res[i + 1] == 1 ? path1[i + 1] : path0[i + 1];
    return tf_select;
}

float median_of_5f(const float* x) {
    float t0, t1, t2 = x[2], t3, t4;
    if (x[0] > x[1]) {
        t0 = x[1];
        t1 = x[0];
    } else {
        t0 = x[0];
        t1 = x[1];
    }
    if (x[3] > x[4]) {
        t3 = x[4];
        t4 = x[3];
    } else {
        t3 = x[3];
        t4 = x[4];
    }
    if (t0 > t3) {
        std::swap(t0, t3);
        std::swap(t1, t4);
    }
    if (t2 > t1) return t1 < t3 ? std::min(t2, t3) : std::min(t4, t1);
    return t2 < t3 ? std::min(t1, t3) : std::min(t2, t4);
}

float median_of_3f(const float* x) {
    float t0, t1, t2 = x[2];
    if (x[0] > x[1]) {
        t0 = x[1];
        t1 = x[0];
    } else {
        t0 = x[0];
        t1 = x[1];
    }
    if (t1 < t2) return t1;
    return t0 < t2 ? t2 : t0;
}

// Dynamic-allocation analysis (reference float path, CBR, no surround /
// tonality analyzer / lfe). Produces per-band boost counts in offsets[]
// (quanta flags for the dynalloc wire loop), importance[] for
// tf_analysis, and spread_weight[] for the spread decision. The
// follower tracks a smoothed spectral floor; boosts go to bands that
// poke above it (tonal peaks the plain allocation starves).
void dynalloc_analysis(const float* band_log_e, const float* band_log_e2,
                       const float* old_band_e, int end, int C,
                       int* offsets, int lsb_depth, int is_transient,
                       int lm, int effective_bytes, int* importance,
                       int* spread_weight) {
    float follower[2 * kNbEBands], noise_floor[kNbEBands];
    float band_log_e3[kNbEBands];
    std::memset(offsets, 0, kNbEBands * sizeof(int));
    float max_depth = -31.9f;
    for (int i = 0; i < end; i++) {
        // eMeans, bit depth, band width and the preemphasis tilt
        // (~square of the bark band id) shape the noise floor.
        noise_floor[i] =
            0.0625f * celt::kLogN[i] + 0.5f + (9 - lsb_depth) -
            static_cast<float>(celt::kEMeans[i]) +
            0.0062f * (i + 5) * (i + 5);
    }
    for (int c = 0; c < C; c++)
        for (int i = 0; i < end; i++)
            max_depth = std::max(
                max_depth, band_log_e[c * kNbEBands + i] - noise_floor[i]);
    {
        // Simple masking model for the spreading decision only.
        float mask[kNbEBands], sig[kNbEBands];
        for (int i = 0; i < end; i++)
            mask[i] = band_log_e[i] - noise_floor[i];
        if (C == 2)
            for (int i = 0; i < end; i++)
                mask[i] = std::max(
                    mask[i], band_log_e[kNbEBands + i] - noise_floor[i]);
        std::memcpy(sig, mask, end * sizeof(float));
        for (int i = 1; i < end; i++)
            mask[i] = std::max(mask[i], mask[i - 1] - 2.0f);
        for (int i = end - 2; i >= 0; i--)
            mask[i] = std::max(mask[i], mask[i + 1] - 3.0f);
        for (int i = 0; i < end; i++) {
            float smr =
                sig[i] - std::max(std::max(0.0f, max_depth - 12.0f),
                                  mask[i]);
            int shift = imin(
                5, imax(0, -static_cast<int>(std::floor(0.5f + smr))));
            spread_weight[i] = 32 >> shift;
        }
    }
    // Feature floor: 24 kb/s at 20 ms up to 96 kb/s at 2.5 ms.
    if (effective_bytes >= 30 + 5 * lm) {
        int last = 0;
        for (int c = 0; c < C; c++) {
            std::memcpy(band_log_e3, &band_log_e2[c * kNbEBands],
                        end * sizeof(float));
            if (lm == 0) {
                // One-bin bands: energy too noisy, widen with history.
                for (int i = 0; i < imin(8, end); i++)
                    band_log_e3[i] =
                        std::max(band_log_e2[c * kNbEBands + i],
                                 old_band_e[c * kNbEBands + i]);
            }
            float* f = &follower[c * kNbEBands];
            f[0] = band_log_e3[0];
            for (int i = 1; i < end; i++) {
                // The last band >=.5 dB above its neighbor bounds the
                // backward pass (bandlimited-signal guard).
                if (band_log_e3[i] > band_log_e3[i - 1] + 0.5f) last = i;
                f[i] = std::min(f[i - 1] + 1.5f, band_log_e3[i]);
            }
            for (int i = last - 1; i >= 0; i--)
                f[i] = std::min(
                    f[i], std::min(f[i + 1] + 2.0f, band_log_e3[i]));
            // Median filter (offset 1 dB) against spurious triggers.
            const float offset = 1.0f;
            for (int i = 2; i < end - 2; i++)
                f[i] = std::max(f[i],
                                median_of_5f(&band_log_e3[i - 2]) - offset);
            float tmp = median_of_3f(&band_log_e3[0]) - offset;
            f[0] = std::max(f[0], tmp);
            f[1] = std::max(f[1], tmp);
            tmp = median_of_3f(&band_log_e3[end - 3]) - offset;
            f[end - 2] = std::max(f[end - 2], tmp);
            f[end - 1] = std::max(f[end - 1], tmp);
            for (int i = 0; i < end; i++)
                f[i] = std::max(f[i], noise_floor[i]);
        }
        if (C == 2) {
            for (int i = 0; i < end; i++) {
                // 24 dB (4 log2) cross-talk between channels.
                follower[kNbEBands + i] = std::max(
                    follower[kNbEBands + i], follower[i] - 4.0f);
                follower[i] = std::max(follower[i],
                                       follower[kNbEBands + i] - 4.0f);
                follower[i] =
                    0.5f *
                    (std::max(0.0f, band_log_e[i] - follower[i]) +
                     std::max(0.0f,
                              band_log_e[kNbEBands + i] -
                                  follower[kNbEBands + i]));
            }
        } else {
            for (int i = 0; i < end; i++)
                follower[i] =
                    std::max(0.0f, band_log_e[i] - follower[i]);
        }
        for (int i = 0; i < end; i++)
            importance[i] = static_cast<int>(std::floor(
                0.5f + 13.0f * std::exp2(std::min(follower[i], 4.0f))));
        // CBR non-transient frames: halve the dynalloc contribution.
        if (!is_transient)
            for (int i = 0; i < end; i++) follower[i] *= 0.5f;
        for (int i = 0; i < end; i++) {
            if (i < 8) follower[i] *= 2;
            if (i >= 12) follower[i] *= 0.5f;
        }
        int32_t tot_boost = 0;
        for (int i = 0; i < end; i++) {
            follower[i] = std::min(follower[i], 4.0f);
            int width = C * (kEBands[i + 1] - kEBands[i]) << lm;
            int boost, boost_bits;
            if (width < 6) {
                boost = static_cast<int>(follower[i]);
                boost_bits = (boost * width) << kBitRes;
            } else if (width > 48) {
                boost = static_cast<int>(follower[i] * 8);
                boost_bits = ((boost * width) << kBitRes) / 8;
            } else {
                boost = static_cast<int>(follower[i] * width / 6);
                boost_bits = (boost * 6) << kBitRes;
            }
            // CBR: dynalloc limited to 2/3 of the frame's bits.
            if ((tot_boost + boost_bits) >> kBitRes >> 3 >
                2 * effective_bytes / 3) {
                int32_t cap = (2 * effective_bytes / 3) << kBitRes << 3;
                offsets[i] = cap - tot_boost;
                tot_boost = cap;
                break;
            }
            offsets[i] = boost;
            tot_boost += boost_bits;
        }
    } else {
        for (int i = 0; i < end; i++) importance[i] = 13;
    }
}

// Allocation-trim analysis (reference float path, no surround / no
// tonality analyzer): base 5 shaded by the low-band stereo correlation
// (correlated stereo -> spend low), the spectral tilt of the band log
// energies (dark spectra -> trim up), and the transient strength.
// Policy-only: the chosen trim is wire-coded and the decoder follows.
int alloc_trim_analysis(const float* X, const float* band_log_e, int end,
                        int lm, int C, int n0, double tf_estimate,
                        int intensity, int32_t equiv_rate) {
    double trim = 5.0;
    if (equiv_rate < 64000) {
        trim = 4.0;
    } else if (equiv_rate < 80000) {
        int32_t frac = (equiv_rate - 64000) >> 10;
        trim = 4.0 + (1.0 / 16.0) * frac;
    }
    if (C == 2) {
        // Inter-channel correlation of the normalized low bands.
        double sum = 0;
        for (int i = 0; i < 8; i++) {
            double partial = 0;
            for (int j = kEBands[i] << lm; j < kEBands[i + 1] << lm; j++)
                partial += static_cast<double>(X[j]) * X[n0 + j];
            sum += partial;
        }
        sum = std::min(1.0, std::fabs(sum * (1.0 / 8)));
        double min_xc = sum;
        for (int i = 8; i < intensity; i++) {
            double partial = 0;
            for (int j = kEBands[i] << lm; j < kEBands[i + 1] << lm; j++)
                partial += static_cast<double>(X[j]) * X[n0 + j];
            min_xc = std::min(min_xc, std::fabs(partial));
        }
        min_xc = std::min(1.0, min_xc);
        double log_xc = std::log2(1.001 - sum * sum);
        trim += std::max(-4.0, 0.75 * log_xc);
    }
    // Spectral tilt of the band log energies.
    double diff = 0;
    for (int c = 0; c < C; c++)
        for (int i = 0; i < end - 1; i++)
            diff += band_log_e[i + c * kNbEBands] * (2 + 2 * i - end);
    diff /= C * (end - 1);
    trim -= std::max(-2.0, std::min(2.0, (diff + 1.0) / 6));
    trim -= 2 * tf_estimate;
    int trim_index = static_cast<int>(std::floor(0.5 + trim));
    return imax(0, imin(10, trim_index));
}
}  // namespace

void CeltEncoder::init(int channels) {
    channels_ = channels;
    final_range_ = 0;
    std::memset(in_mem_, 0, sizeof(in_mem_));
    std::memset(prefilter_mem_, 0, sizeof(prefilter_mem_));
    std::memset(old_ebands_, 0, sizeof(old_ebands_));
    std::memset(energy_error_, 0, sizeof(energy_error_));
    preemph_mem_[0] = preemph_mem_[1] = 0;
    delayed_intra_ = 1.0f;
    last_coded_bands_ = 0;
    prefilter_period_ = 0;
    prefilter_gain_ = 0;
    prefilter_tapset_ = 0;
    mdct_window_fill(window_, kOverlap);
}

int CeltEncoder::encode_frame(const float* pcm, int frame_size,
                              uint8_t* out, int nbytes) {
    const int C = channels_;
    const int start = 0;
    const int end = kNbEBands;
    int lm;
    for (lm = 0; lm <= 3; lm++)
        if (120 << lm == frame_size) break;
    if (lm > 3 || nbytes < 2 || nbytes > 1275) return -1;
    const int m = 1 << lm;
    const int n = frame_size;

    RangeEncoder enc;
    enc.init(out, static_cast<uint32_t>(nbytes));
    const int32_t total_bits = nbytes * 8;

    // Pre-emphasis (reference signal scale), raw samples after the
    // filtered overlap history.
    static thread_local double in[2][kOverlap + kMaxFrame];
    for (int c = 0; c < C; c++) {
        double* inp = in[c] + kOverlap;
        float mem = preemph_mem_[c];
        for (int i = 0; i < n; i++) {
            float x = pcm[C * i + c] * 32768.0f;
            inp[i] = x - mem;
            mem = kPreemphCoef * x;
        }
        preemph_mem_[c] = mem;
    }

    // ---- Pitch prefilter (the decoder re-adds it as the postfilter) ----
    int pf_on = 0, pitch_index = kCombFilterMinPeriodEnc, qg = 0;
    double gain1 = 0;
    const int new_tapset = 0;
    {
        // Unfiltered history + this frame's raw samples.
        static thread_local double pre[2][kCombMaxPeriod + kMaxFrame];
        for (int c = 0; c < C; c++) {
            std::memcpy(pre[c], prefilter_mem_[c],
                        kCombMaxPeriod * sizeof(double));
            std::memcpy(pre[c] + kCombMaxPeriod, in[c] + kOverlap,
                        n * sizeof(double));
        }
        const bool enabled = nbytes > 12 * C && start == 0;
        if (enabled) {
            double pitch_buf[(kCombMaxPeriod + kMaxFrame) / 2];
            const double* chans[2] = { pre[0], pre[1] };
            pitch::pitch_downsample(chans, pitch_buf, kCombMaxPeriod + n,
                                    C);
            // Skip the shortest 1.5 octaves (short-term correlation traps).
            pitch::pitch_search(pitch_buf + (kCombMaxPeriod >> 1),
                                pitch_buf, n,
                                kCombMaxPeriod -
                                    3 * kCombFilterMinPeriodEnc,
                                &pitch_index);
            pitch_index = kCombMaxPeriod - pitch_index;
            gain1 = pitch::remove_doubling(
                pitch_buf, kCombMaxPeriod, kCombFilterMinPeriodEnc, n,
                &pitch_index, prefilter_period_, prefilter_gain_);
            if (pitch_index > kCombMaxPeriod - 2)
                pitch_index = kCombMaxPeriod - 2;
            gain1 = 0.7 * gain1;
        }
        // Rate/continuity-adjusted enabling threshold.
        double pf_threshold = 0.2;
        int dp = pitch_index - prefilter_period_;
        if ((dp < 0 ? -dp : dp) * 10 > pitch_index) pf_threshold += 0.2;
        if (nbytes < 25) pf_threshold += 0.1;
        if (nbytes < 35) pf_threshold += 0.1;
        if (prefilter_gain_ > 0.4) pf_threshold -= 0.1;
        if (prefilter_gain_ > 0.55) pf_threshold -= 0.1;
        if (pf_threshold < 0.2) pf_threshold = 0.2;
        if (gain1 < pf_threshold) {
            gain1 = 0;
            pf_on = 0;
            qg = 0;
        } else {
            double dg = gain1 - prefilter_gain_;
            if ((dg < 0 ? -dg : dg) < 0.1) gain1 = prefilter_gain_;
            qg = static_cast<int>(std::floor(0.5 + gain1 * 32.0 / 3.0)) - 1;
            qg = qg < 0 ? 0 : (qg > 7 ? 7 : qg);
            gain1 = 0.09375 * (qg + 1);
            pf_on = 1;
        }
        // Apply: fade the previous frame's filter into this one over the
        // window, writing the FILTERED frame after the filtered history.
        for (int c = 0; c < C; c++) {
            prefilter_period_ =
                prefilter_period_ > kCombFilterMinPeriodEnc
                    ? prefilter_period_
                    : kCombFilterMinPeriodEnc;
            std::memcpy(in[c], in_mem_[c], kOverlap * sizeof(double));
            comb_filter_shared(in[c] + kOverlap, pre[c] + kCombMaxPeriod,
                               prefilter_period_, pitch_index, n,
                               -prefilter_gain_, -gain1,
                               prefilter_tapset_, new_tapset, window_,
                               kOverlap);
            std::memcpy(in_mem_[c], in[c] + n, kOverlap * sizeof(double));
            // Unfiltered history for the next frame's analysis.
            if (n >= kCombMaxPeriod) {
                std::memcpy(prefilter_mem_[c], pre[c] + n,
                            kCombMaxPeriod * sizeof(double));
            } else {
                std::memmove(prefilter_mem_[c], prefilter_mem_[c] + n,
                             (kCombMaxPeriod - n) * sizeof(double));
                std::memcpy(prefilter_mem_[c] + kCombMaxPeriod - n,
                            pre[c] + kCombMaxPeriod, n * sizeof(double));
            }
        }
    }

    // Transient detection on the (prefiltered) input incl. history.
    // Gate on the bit budget BEFORE the MDCT: the flag on the wire, the
    // transform interleave and the decoder's tf_res derivation must all
    // agree, so is_transient may never change after this point.
    double tf_estimate = 0;
    int tf_chan = 0;
    int is_transient =
        lm > 0 ? transient_analysis(&in[0][0], n + kOverlap, C,
                                    &tf_estimate, &tf_chan)
               : 0;
    if (!(lm > 0 && static_cast<int32_t>(enc.tell()) + 3 <= total_bits))
        is_transient = 0;
    if (std::getenv("GLINT_DBG_TRANSIENT"))
        std::fprintf(stderr, "T%d", is_transient);
    const int short_blocks = is_transient ? m : 0;

    // Forward MDCT: one long transform, or M interleaved short blocks.
    static thread_local float X[2 * kMaxFrame];
    {
        double freq_d[kMaxFrame];
        int B = is_transient ? m : 1;
        int nb = is_transient ? 120 : n;
        int shift = is_transient ? 3 : 3 - lm;
        for (int c = 0; c < C; c++) {
            for (int b = 0; b < B; b++)
                mdct_.forward(in[c] + b * nb, &freq_d[b], window_,
                              kOverlap, shift, B);
            for (int k = 0; k < n; k++)
                X[c * n + k] = static_cast<float>(freq_d[k]);
        }
    }

    // Band analysis. band_log_e2 = LONG-window log energies (+lm/2 dB)
    // even on transient frames — dynalloc's follower needs the stable
    // long-window view (the reference's secondMdct at complexity >= 8).
    float band_e[2 * kNbEBands], band_log_e[2 * kNbEBands];
    float band_log_e2[2 * kNbEBands];
    float x_norm[2 * kMaxFrame];
    compute_band_energies(X, band_e, end, C, lm);
    amp2Log2(end, end, band_e, band_log_e, C);
    if (is_transient) {
        static thread_local float x_long[2 * kMaxFrame];
        double freq_d[kMaxFrame];
        float band_e2[2 * kNbEBands];
        for (int c = 0; c < C; c++) {
            mdct_.forward(in[c], freq_d, window_, kOverlap, 3 - lm, 1);
            for (int k = 0; k < n; k++)
                x_long[c * n + k] = static_cast<float>(freq_d[k]);
        }
        compute_band_energies(x_long, band_e2, end, C, lm);
        amp2Log2(end, end, band_e2, band_log_e2, C);
        for (int i = 0; i < C * kNbEBands; i++)
            band_log_e2[i] += 0.5f * lm;
    } else {
        std::memcpy(band_log_e2, band_log_e, sizeof(band_log_e2));
    }
    normalise_bands(X, x_norm, band_e, end, C, m);

    // Dynalloc analysis: per-band boost counts (wire-coded below),
    // importance for tf_analysis, spread weights (spread analysis TBD).
    int offsets[kNbEBands], importance[kNbEBands];
    int spread_weight[kNbEBands];
    dynalloc_analysis(band_log_e, band_log_e2, old_ebands_, end, C,
                      offsets, /*lsb_depth=*/16, is_transient, lm, nbytes,
                      importance, spread_weight);

    // Per-band TF resolution (quality item 3). Disabled at very small
    // frames, like the reference.
    int tf_res[kNbEBands];
    int tf_select = 0;
    if (nbytes >= 15 * C) {
        int lambda = imax(80, 20480 / nbytes + 2);
        tf_select = tf_analysis(end, is_transient, tf_res, lambda, x_norm,
                                n, lm, tf_estimate, tf_chan, importance);
    } else {
        for (int i = 0; i < end; i++) tf_res[i] = is_transient;
    }

    // ---- Symbol sequence (mirrors the conformant decoder) ----
    int32_t tell = static_cast<int32_t>(enc.tell());
    if (tell == 1) enc.enc_bit_logp(0, 15);  // not silence
    if (start == 0 && tell + 16 <= total_bits) {
        enc.enc_bit_logp(pf_on, 1);
        if (pf_on) {
            pitch_index += 1;
            int octave = ec::ilog(static_cast<uint32_t>(pitch_index)) - 5;
            enc.enc_uint(static_cast<uint32_t>(octave), 6);
            enc.enc_bits(
                static_cast<uint32_t>(pitch_index - (16 << octave)),
                static_cast<unsigned>(4 + octave));
            pitch_index -= 1;
            enc.enc_bits(static_cast<uint32_t>(qg), 3);
            enc.enc_icdf(new_tapset, celt::kTapsetIcdf, 2);
        }
    }
    // Filter state rotation (must mirror what the decoder's postfilter
    // state does).
    prefilter_period_ = pitch_index;
    prefilter_gain_ = gain1;
    prefilter_tapset_ = new_tapset;
    tell = static_cast<int32_t>(enc.tell());
    if (lm > 0 && tell + 3 <= total_bits) {
        enc.enc_bit_logp(is_transient, 3);
        tell = static_cast<int32_t>(enc.tell());
    }

    // Coarse energy (two-pass intra/inter decided inside).
    quant_coarse_energy(start, end, end, band_log_e, old_ebands_,
                        static_cast<uint32_t>(total_bits), energy_error_,
                        enc, C, lm, nbytes, /*force_intra=*/0,
                        &delayed_intra_, /*two_pass=*/1, /*loss_rate=*/0,
                        /*lfe=*/0);

    // tf_encode: XOR-delta-code the per-band change bits where the
    // budget allows, then the select bit when it would actually change
    // the decode. After encoding, tf_res is remapped through the select
    // table EXACTLY as the decoder's tf_decode does — quant_all_bands
    // must see the same per-band tf resolution the decoder will derive,
    // or the band symbols desync (valid wire, garbage audio).
    {
        uint32_t budget = static_cast<uint32_t>(total_bits);
        uint32_t tf_tell = enc.tell();
        int logp = is_transient ? 2 : 4;
        uint32_t tf_select_rsv = lm > 0 && tf_tell + logp + 1 <= budget;
        budget -= tf_select_rsv;
        int curr = 0, tf_changed = 0;
        for (int i = start; i < end; i++) {
            if (tf_tell + logp <= budget) {
                enc.enc_bit_logp(tf_res[i] ^ curr,
                                 static_cast<unsigned>(logp));
                tf_tell = enc.tell();
                curr = tf_res[i];
                tf_changed |= curr;
            } else {
                tf_res[i] = curr;
            }
            logp = is_transient ? 4 : 5;
        }
        if (tf_select_rsv &&
            kTfSelectTable[lm][4 * is_transient + tf_changed] !=
                kTfSelectTable[lm][4 * is_transient + 2 + tf_changed])
            enc.enc_bit_logp(static_cast<unsigned>(tf_select), 1);
        else
            tf_select = 0;
        for (int i = start; i < end; i++)
            tf_res[i] = kTfSelectTable[lm][4 * is_transient +
                                           2 * tf_select + tf_res[i]];
    }

    tell = static_cast<int32_t>(enc.tell());
    int spread = kSpreadNormal;
    if (tell + 4 <= total_bits) enc.enc_icdf(spread, celt::kSpreadIcdf, 5);

    int cap[kNbEBands];
    init_caps(cap, lm, C);

    // Dynalloc wire loop: one flag chain per band; each accepted flag
    // buys `quanta` eighth-bits of boost. offsets[] comes in as the
    // analysis' flag counts and leaves as the wire-coded boost in
    // eighth-bits — exactly what the decoder derives and what
    // compute_allocation consumes.
    int32_t total_boost = 0;
    {
        int dynalloc_logp = 6;
        int32_t total_bits_q3 = total_bits << kBitRes;
        int32_t tellf = static_cast<int32_t>(enc.tell_frac());
        for (int i = start; i < end; i++) {
            int width = C * (kEBands[i + 1] - kEBands[i]) << lm;
            // 6 bits per quantum, capped at 1 bit/sample, floored at
            // 1/8 bit/sample.
            int quanta = imin(width << kBitRes, imax(6 << kBitRes, width));
            int dynalloc_loop_logp = dynalloc_logp;
            int boost = 0;
            int j;
            for (j = 0;
                 tellf + (dynalloc_loop_logp << kBitRes) <
                     total_bits_q3 - total_boost &&
                 boost < cap[i];
                 j++) {
                int flag = j < offsets[i];
                enc.enc_bit_logp(flag,
                                 static_cast<unsigned>(dynalloc_loop_logp));
                tellf = static_cast<int32_t>(enc.tell_frac());
                if (!flag) break;
                boost += quanta;
                total_boost += quanta;
                dynalloc_loop_logp = 1;
            }
            if (j) dynalloc_logp = imax(2, dynalloc_logp - 1);
            offsets[i] = boost;
        }
    }

    // Trim: analyzed when the budget (net of dynalloc boosts) allows
    // the symbol, else the decoder-side default 5.
    int alloc_trim = 5;
    if (static_cast<int32_t>(enc.tell_frac()) + (6 << kBitRes) <=
        (total_bits << kBitRes) - total_boost) {
        // equiv_rate: 20ms-equivalent bits/s (shorter frames discount
        // their fixed per-frame overhead, like the reference).
        int32_t equiv_rate =
            ((static_cast<int32_t>(nbytes) * 8 * 50) >> (3 - lm)) -
            (40 * C + 20) * ((400 >> lm) - 50);
        alloc_trim = alloc_trim_analysis(x_norm, band_log_e, end, lm, C,
                                         n, tf_estimate, end, equiv_rate);
        enc.enc_icdf(alloc_trim, celt::kTrimIcdf, 7);
    }

    int32_t bits = ((static_cast<int32_t>(nbytes) * 8) << kBitRes) -
                   static_cast<int32_t>(enc.tell_frac()) - 1;
    int anti_collapse_rsv =
        is_transient && lm >= 2 && bits >= ((lm + 2) << kBitRes)
            ? 1 << kBitRes
            : 0;
    bits -= anti_collapse_rsv;

    int pulses[kNbEBands], fine_quant[kNbEBands], fine_priority[kNbEBands];
    int intensity = end;  // no intensity stereo
    int dual_stereo = 0;
    int32_t balance = 0;
    int coded_bands = compute_allocation_enc(
        start, end, offsets, cap, alloc_trim, &intensity, &dual_stereo,
        bits, &balance, pulses, fine_quant, fine_priority, C, lm, enc,
        last_coded_bands_, end - 1);
    last_coded_bands_ = coded_bands;

    quant_fine_energy(start, end, old_ebands_, energy_error_, fine_quant,
                      enc, C);

    uint8_t collapse_masks[2 * kNbEBands];
    uint32_t seed = 0;
    quant_all_bands_enc(start, end, x_norm, C == 2 ? x_norm + n : nullptr,
                        collapse_masks, band_e, pulses, short_blocks,
                        spread, dual_stereo, intensity, tf_res,
                        ((static_cast<int32_t>(nbytes) * 8) << kBitRes) -
                            anti_collapse_rsv,
                        balance, enc, lm, coded_bands, &seed);

    if (anti_collapse_rsv > 0)
        enc.enc_bits(consec_transient_ < 2 ? 1u : 0u, 1);

    quant_energy_finalise(start, end, old_ebands_, energy_error_,
                          fine_quant, fine_priority,
                          nbytes * 8 - static_cast<int>(enc.tell()), enc,
                          C);

    if (is_transient)
        consec_transient_++;
    else
        consec_transient_ = 0;

    final_range_ = enc.range();
    enc.done();
    if (enc.error()) return -2;
    (void)imax;
    (void)imin;
    return nbytes;
}

}  // namespace opus
}  // namespace glint
