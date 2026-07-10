// CELT frame decoder — RFC 6716 section 4.3
// MIT License - Clean-room implementation

#include "opus_celt_decoder.hpp"

#include <cmath>
#include <cstring>

#include "opus_celt_bands.hpp"
#include "opus_celt_energy.hpp"
#include "opus_celt_rate.hpp"
#include "opus_celt_tables.hpp"

namespace glint {
namespace opus {

namespace {

using celt::kEBands;
using celt::kNbEBands;

constexpr int kCombFilterMinPeriod = 15;
constexpr double kPreemphCoef = 0.85;  // 48 kHz mode
constexpr double kVerySmall = 1e-30;
constexpr int kSpreadNormal = 2;

inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }

// TF resolution change per (LM; transient, tf_select, per-band bit).
const signed char kTfSelectTable[4][8] = {
    { 0, -1, 0, -1, 0, -1, 0, -1 },
    { 0, -1, 0, -2, 1, 0, 1, -1 },
    { 0, -2, 0, -3, 2, 0, 1, -1 },
    { 0, -2, 0, -3, 3, 0, 1, -1 },
};

void tf_decode(int start, int end, int is_transient, int* tf_res, int lm,
               RangeDecoder& dec) {
    uint32_t budget = dec.storage_bytes() * 8;
    uint32_t tell = dec.tell();
    int logp = is_transient ? 2 : 4;
    // One reserved bit switches between the two halves of the table.
    int tf_select_rsv = lm > 0 && tell + logp + 1 <= budget;
    budget -= static_cast<uint32_t>(tf_select_rsv);
    int tf_changed = 0;
    int curr = 0;
    for (int i = start; i < end; i++) {
        if (tell + logp <= budget) {
            curr ^= dec.dec_bit_logp(static_cast<unsigned>(logp));
            tell = dec.tell();
            tf_changed |= curr;
        }
        tf_res[i] = curr;
        logp = is_transient ? 4 : 5;
    }
    int tf_select = 0;
    if (tf_select_rsv &&
        kTfSelectTable[lm][4 * is_transient + 0 + tf_changed] !=
            kTfSelectTable[lm][4 * is_transient + 2 + tf_changed]) {
        tf_select = dec.dec_bit_logp(1);
    }
    for (int i = start; i < end; i++)
        tf_res[i] =
            kTfSelectTable[lm][4 * is_transient + 2 * tf_select + tf_res[i]];
}

// Expand normalized bands back to signal scale: gain = 2^(logE + eMeans).
void denormalise_bands(const double* X, double* freq, const double* band_log_e,
                       int start, int end, int m, int n, int silence) {
    int bound = m * kEBands[end];
    if (silence) {
        bound = 0;
        start = end = 0;
    }
    double* f = freq;
    const double* x = X + m * kEBands[start];
    for (int i = 0; i < m * kEBands[start]; i++) *f++ = 0;
    for (int i = start; i < end; i++) {
        int j = m * kEBands[i];
        int band_end = m * kEBands[i + 1];
        double lg = band_log_e[i] + celt::kEMeans[i];
        double g = std::exp2(lg < 32.0 ? lg : 32.0);
        do {
            *f++ = *x++ * g;
        } while (++j < band_end);
    }
    std::memset(freq + bound, 0,
                static_cast<size_t>(n - bound) * sizeof(double));
}

// Pitch postfilter: 5-tap comb at period T, cross-faded over the overlap
// from the previous frame's filter parameters.
void comb_filter(double* y, double* x, int t0, int t1, int n, double g0,
                 double g1, int tapset0, int tapset1, const double* window,
                 int overlap) {
    static const double kGains[3][3] = {
        { 0.3066406250, 0.2170410156, 0.1296386719 },
        { 0.4638671875, 0.2680664062, 0.0 },
        { 0.7998046875, 0.1000976562, 0.0 },
    };
    if (g0 == 0 && g1 == 0) {
        if (x != y) std::memmove(y, x, n * sizeof(double));
        return;
    }
    t0 = imax(t0, kCombFilterMinPeriod);
    t1 = imax(t1, kCombFilterMinPeriod);
    double g00 = g0 * kGains[tapset0][0];
    double g01 = g0 * kGains[tapset0][1];
    double g02 = g0 * kGains[tapset0][2];
    double g10 = g1 * kGains[tapset1][0];
    double g11 = g1 * kGains[tapset1][1];
    double g12 = g1 * kGains[tapset1][2];
    double x1 = x[-t1 + 1];
    double x2 = x[-t1];
    double x3 = x[-t1 - 1];
    double x4 = x[-t1 - 2];
    if (g0 == g1 && t0 == t1 && tapset0 == tapset1) overlap = 0;
    int i;
    for (i = 0; i < overlap; i++) {
        double x0 = x[i - t1 + 2];
        double f = window[i] * window[i];
        y[i] = x[i] + (1 - f) * g00 * x[i - t0] +
               (1 - f) * g01 * (x[i - t0 + 1] + x[i - t0 - 1]) +
               (1 - f) * g02 * (x[i - t0 + 2] + x[i - t0 - 2]) +
               f * g10 * x2 + f * g11 * (x1 + x3) + f * g12 * (x0 + x4);
        x4 = x3;
        x3 = x2;
        x2 = x1;
        x1 = x0;
    }
    if (g1 == 0) {
        if (x != y)
            std::memmove(y + overlap, x + overlap,
                         (n - overlap) * sizeof(double));
        return;
    }
    // Constant-parameter tail.
    x1 = x[i - t1 + 1];
    x2 = x[i - t1];
    x3 = x[i - t1 - 1];
    x4 = x[i - t1 - 2];
    for (; i < n; i++) {
        double x0 = x[i - t1 + 2];
        y[i] = x[i] + g10 * x2 + g11 * (x1 + x3) + g12 * (x0 + x4);
        x4 = x3;
        x3 = x2;
        x2 = x1;
        x1 = x0;
    }
}

}  // namespace

void CeltDecoder::init(int channels) {
    channels_ = channels;
    rng_ = 0;
    std::memset(decode_mem_, 0, sizeof(decode_mem_));
    std::memset(old_ebands_, 0, sizeof(old_ebands_));
    for (int i = 0; i < 2 * kNbEBands; i++)
        old_log_e_[i] = old_log_e2_[i] = background_log_e_[i] = -28.0;
    preemph_mem_[0] = preemph_mem_[1] = 0;
    postfilter_period_ = postfilter_period_old_ = 0;
    postfilter_gain_ = postfilter_gain_old_ = 0;
    postfilter_tapset_ = postfilter_tapset_old_ = 0;
    mdct_window_fill(window_, kOverlap);
}

void CeltDecoder::synthesis(const double* X, int CC, int C,
                            int is_transient, int lm, int silence,
                            int effend, int start) {
    int n = 120 << lm;
    int b, nb, shift;
    if (is_transient) {
        b = 1 << lm;
        nb = 120;
        shift = 3;  // maxLM
    } else {
        b = 1;
        nb = 120 << lm;
        shift = 3 - lm;
    }
    double freq[kMaxFrame];
    if (CC == 2 && C == 1) {
        // Mono stream to stereo output: same signal into both channels.
        // The IMDCT destroys its input, so stage a copy inside channel 1's
        // output region (in-place safe: input == output + overlap/2).
        denormalise_bands(X, freq, old_ebands_, start, effend, 1 << lm, n,
                          silence);
        double* out0 = decode_mem_[0] + kDecodeBufferSize - n;
        double* out1 = decode_mem_[1] + kDecodeBufferSize - n;
        double* freq2 = out1 + kOverlap / 2;
        std::memcpy(freq2, freq, n * sizeof(double));
        for (int blk = 0; blk < b; blk++)
            imdct_.backward(&freq2[blk], out0 + nb * blk, window_, kOverlap,
                            shift, b);
        for (int blk = 0; blk < b; blk++)
            imdct_.backward(&freq[blk], out1 + nb * blk, window_, kOverlap,
                            shift, b);
    } else if (CC == 1 && C == 2) {
        // Stereo stream to mono output: average the denormalised spectra.
        double* out0 = decode_mem_[0] + kDecodeBufferSize - n;
        double* freq2 = out0 + kOverlap / 2;
        denormalise_bands(X, freq, old_ebands_, start, effend, 1 << lm, n,
                          silence);
        denormalise_bands(X + n, freq2, old_ebands_ + kNbEBands, start,
                          effend, 1 << lm, n, silence);
        for (int i = 0; i < n; i++) freq[i] = 0.5 * freq[i] + 0.5 * freq2[i];
        for (int blk = 0; blk < b; blk++)
            imdct_.backward(&freq[blk], out0 + nb * blk, window_, kOverlap,
                            shift, b);
    } else {
        for (int c = 0; c < CC; c++) {
            double* out_syn = decode_mem_[c] + kDecodeBufferSize - n;
            denormalise_bands(X + c * n, freq, old_ebands_ + c * kNbEBands,
                              0, effend, 1 << lm, n, silence);
            for (int blk = 0; blk < b; blk++)
                imdct_.backward(&freq[blk], out_syn + nb * blk, window_,
                                kOverlap, shift, b);
        }
    }
}

int CeltDecoder::decode_frame(RangeDecoder& dec, uint32_t payload_bytes,
                              float* pcm, int frame_size,
                              int stream_channels, int end_band,
                              int start_band) {
    const int CC = channels_;       // decoder output channels
    const int C = stream_channels;  // channels coded in the stream
    const int start = start_band;
    const int end = end_band;
    const int effend = end;

    int lm;
    for (lm = 0; lm <= 3; lm++)
        if (120 << lm == frame_size) break;
    if (lm > 3 || payload_bytes < 1 || payload_bytes > 1275) return -1;
    const int m = 1 << lm;
    const int n = frame_size;
    const int32_t len = static_cast<int32_t>(payload_bytes);

    double* old_ebands = old_ebands_;
    if (C == 1) {
        for (int i = 0; i < kNbEBands; i++)
            old_ebands[i] = old_ebands[i] > old_ebands[kNbEBands + i]
                                ? old_ebands[i]
                                : old_ebands[kNbEBands + i];
    }

    int32_t total_bits = len * 8;
    int32_t tell = static_cast<int32_t>(dec.tell());

    // Silence flag (only meaningful as the first symbol).
    int silence;
    if (tell >= total_bits)
        silence = 1;
    else if (tell == 1)
        silence = dec.dec_bit_logp(15);
    else
        silence = 0;
    if (silence) {
        tell = total_bits;
        dec.set_tell(static_cast<uint32_t>(tell));
    }

    // Postfilter parameters.
    double postfilter_gain = 0;
    int postfilter_pitch = 0;
    int postfilter_tapset = 0;
    if (start == 0 && tell + 16 <= total_bits) {
        if (dec.dec_bit_logp(1)) {
            int octave = static_cast<int>(dec.dec_uint(6));
            postfilter_pitch =
                (16 << octave) +
                static_cast<int>(dec.dec_bits(4 + octave)) - 1;
            int qg = static_cast<int>(dec.dec_bits(3));
            if (static_cast<int32_t>(dec.tell()) + 2 <= total_bits)
                postfilter_tapset = dec.dec_icdf(celt::kTapsetIcdf, 2);
            postfilter_gain = 0.09375 * (qg + 1);
        }
        tell = static_cast<int32_t>(dec.tell());
    }

    int is_transient = 0;
    if (lm > 0 && tell + 3 <= total_bits) {
        is_transient = dec.dec_bit_logp(3);
        tell = static_cast<int32_t>(dec.tell());
    }
    int short_blocks = is_transient ? m : 0;

    int intra_ener = tell + 3 <= total_bits ? dec.dec_bit_logp(3) : 0;

    unquant_coarse_energy(start, end, old_ebands, intra_ener != 0, dec, C,
                          lm);

    int tf_res[kNbEBands];
    tf_decode(start, end, is_transient, tf_res, lm, dec);

    tell = static_cast<int32_t>(dec.tell());
    int spread = kSpreadNormal;
    if (tell + 4 <= total_bits) spread = dec.dec_icdf(celt::kSpreadIcdf, 5);

    int cap[kNbEBands];
    init_caps(cap, lm, C);

    // Dynalloc: per-band boosts, cost halves after the first boost.
    int offsets[kNbEBands];
    int dynalloc_logp = 6;
    total_bits <<= kBitRes;
    int32_t tellf = static_cast<int32_t>(dec.tell_frac());
    for (int i = start; i < end; i++) {
        int width = C * (kEBands[i + 1] - kEBands[i]) << lm;
        // 6 bits/step, within [1/8 bit, 1 bit] per sample.
        int quanta = imin(width << kBitRes, imax(6 << kBitRes, width));
        int dynalloc_loop_logp = dynalloc_logp;
        int boost = 0;
        while (tellf + (dynalloc_loop_logp << kBitRes) < total_bits &&
               boost < cap[i]) {
            int flag = dec.dec_bit_logp(
                static_cast<unsigned>(dynalloc_loop_logp));
            tellf = static_cast<int32_t>(dec.tell_frac());
            if (!flag) break;
            boost += quanta;
            total_bits -= quanta;
            dynalloc_loop_logp = 1;
        }
        offsets[i] = boost;
        if (boost > 0) dynalloc_logp = imax(2, dynalloc_logp - 1);
    }

    int alloc_trim = tellf + (6 << kBitRes) <= total_bits
                         ? dec.dec_icdf(celt::kTrimIcdf, 7)
                         : 5;

    int32_t bits = (len * 8 << kBitRes) -
                   static_cast<int32_t>(dec.tell_frac()) - 1;
    int anti_collapse_rsv =
        is_transient && lm >= 2 && bits >= (lm + 2) << kBitRes ? 1 << kBitRes
                                                               : 0;
    bits -= anti_collapse_rsv;

    int pulses[kNbEBands], fine_quant[kNbEBands], fine_priority[kNbEBands];
    int intensity = 0, dual_stereo = 0;
    int32_t balance = 0;
    int coded_bands = compute_allocation_dec(
        start, end, offsets, cap, alloc_trim, &intensity, &dual_stereo,
        bits, &balance, pulses, fine_quant, fine_priority, C, lm, dec);

    unquant_fine_energy(start, end, old_ebands, fine_quant, dec, C);

    for (int c = 0; c < CC; c++)
        std::memmove(decode_mem_[c], decode_mem_[c] + n,
                     (kDecodeBufferSize - n + kOverlap) * sizeof(double));

    uint8_t collapse_masks[2 * kNbEBands];
    static thread_local double X[2 * kMaxFrame];
    quant_all_bands_dec(start, end, X, C == 2 ? X + n : nullptr,
                        collapse_masks, pulses, short_blocks, spread,
                        dual_stereo, intensity, tf_res,
                        len * (8 << kBitRes) - anti_collapse_rsv, balance,
                        dec, lm, coded_bands, &rng_,
                        C == 1 /* mono decoders disable inversion */);

    int anti_collapse_on = 0;
    if (anti_collapse_rsv > 0)
        anti_collapse_on = static_cast<int>(dec.dec_bits(1));

    unquant_energy_finalise(start, end, old_ebands, fine_quant,
                            fine_priority,
                            len * 8 - static_cast<int>(dec.tell()), dec, C);

    if (anti_collapse_on)
        anti_collapse(X, collapse_masks, lm, C, n, start, end, old_ebands,
                      old_log_e_, old_log_e2_, pulses, rng_);

    if (silence) {
        for (int i = 0; i < 2 * kNbEBands; i++) old_ebands[i] = -28.0;
    }

    synthesis(X, CC, C, is_transient, lm, silence, effend, start);

    // Postfilter: previous frame's parameters over the first short-MDCT,
    // then a cross-fade to this frame's parameters.
    for (int c = 0; c < CC; c++) {
        double* out_syn = decode_mem_[c] + kDecodeBufferSize - n;
        postfilter_period_ = imax(postfilter_period_, kCombFilterMinPeriod);
        postfilter_period_old_ =
            imax(postfilter_period_old_, kCombFilterMinPeriod);
        comb_filter(out_syn, out_syn, postfilter_period_old_,
                    postfilter_period_, 120, postfilter_gain_old_,
                    postfilter_gain_, postfilter_tapset_old_,
                    postfilter_tapset_, window_, kOverlap);
        if (lm != 0)
            comb_filter(out_syn + 120, out_syn + 120, postfilter_period_,
                        postfilter_pitch, n - 120, postfilter_gain_,
                        postfilter_gain, postfilter_tapset_,
                        postfilter_tapset, window_, kOverlap);
    }
    postfilter_period_old_ = postfilter_period_;
    postfilter_gain_old_ = postfilter_gain_;
    postfilter_tapset_old_ = postfilter_tapset_;
    postfilter_period_ = postfilter_pitch;
    postfilter_gain_ = postfilter_gain;
    postfilter_tapset_ = postfilter_tapset;
    if (lm != 0) {
        postfilter_period_old_ = postfilter_period_;
        postfilter_gain_old_ = postfilter_gain_;
        postfilter_tapset_old_ = postfilter_tapset_;
    }

    if (C == 1)
        std::memcpy(&old_ebands[kNbEBands], old_ebands,
                    kNbEBands * sizeof(double));

    // Energy history rotation (anti-collapse depends on two frames back).
    if (!is_transient) {
        std::memcpy(old_log_e2_, old_log_e_, sizeof(old_log_e2_));
        std::memcpy(old_log_e_, old_ebands, sizeof(old_log_e_));
    } else {
        for (int i = 0; i < 2 * kNbEBands; i++)
            old_log_e_[i] =
                old_log_e_[i] < old_ebands[i] ? old_log_e_[i] : old_ebands[i];
    }
    // Noise floor may rise at most 2.4 dB/s.
    double max_bg_incr = imin(160, m) * 0.001;
    for (int i = 0; i < 2 * kNbEBands; i++)
        background_log_e_[i] =
            background_log_e_[i] + max_bg_incr < old_ebands[i]
                ? background_log_e_[i] + max_bg_incr
                : old_ebands[i];
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < start; i++) {
            old_ebands[c * kNbEBands + i] = 0;
            old_log_e_[c * kNbEBands + i] =
                old_log_e2_[c * kNbEBands + i] = -28.0;
        }
        for (int i = end; i < kNbEBands; i++) {
            old_ebands[c * kNbEBands + i] = 0;
            old_log_e_[c * kNbEBands + i] =
                old_log_e2_[c * kNbEBands + i] = -28.0;
        }
    }
    rng_ = dec.range();

    // De-emphasis (1-pole) and PCM output at ±1.0.
    for (int c = 0; c < CC; c++) {
        double* x = decode_mem_[c] + kDecodeBufferSize - n;
        double mem = preemph_mem_[c];
        for (int j = 0; j < n; j++) {
            double tmp = x[j] + kVerySmall + mem;
            mem = kPreemphCoef * tmp;
            pcm[j * CC + c] = static_cast<float>(tmp * (1.0 / 32768.0));
        }
        preemph_mem_[c] = mem;
    }

    if (static_cast<int32_t>(dec.tell()) > 8 * len) return -3;
    return frame_size;
}

}  // namespace opus
}  // namespace glint
