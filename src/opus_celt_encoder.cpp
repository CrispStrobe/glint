// CELT frame encoder — RFC 6716 section 4.3 (encoder side)
// MIT License - Clean-room implementation

#include "opus_celt_encoder.hpp"

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
inline int imin(int a, int b) { return a < b ? a : b; }
inline int imax(int a, int b) { return a > b ? a : b; }
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

    // Forward MDCT (long block), double transform narrowed to float
    // spectra: the layer gates are float-exact, the transform itself only
    // needs to be accurate (validity never depends on it).
    static thread_local float X[2 * kMaxFrame];
    {
        double freq_d[kMaxFrame];
        int shift = 3 - lm;
        for (int c = 0; c < C; c++) {
            mdct_.forward(in[c], freq_d, window_, kOverlap, shift, 1);
            for (int k = 0; k < n; k++)
                X[c * n + k] = static_cast<float>(freq_d[k]);
        }
    }

    // Band analysis.
    float band_e[2 * kNbEBands], band_log_e[2 * kNbEBands];
    float x_norm[2 * kMaxFrame];
    compute_band_energies(X, band_e, end, C, lm);
    amp2Log2(end, end, band_e, band_log_e, C);
    normalise_bands(X, x_norm, band_e, end, C, m);

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
        enc.enc_bit_logp(0, 3);  // not transient
        tell = static_cast<int32_t>(enc.tell());
    }

    // Coarse energy (two-pass intra/inter decided inside).
    quant_coarse_energy(start, end, end, band_log_e, old_ebands_,
                        static_cast<uint32_t>(total_bits), energy_error_,
                        enc, C, lm, nbytes, /*force_intra=*/0,
                        &delayed_intra_, /*two_pass=*/1, /*loss_rate=*/0,
                        /*lfe=*/0);

    // tf_encode with all-zero tf_res: per-band change bits where the
    // budget allows; the select bit is skipped when both table halves
    // agree (they do for the all-zero path).
    {
        uint32_t budget = static_cast<uint32_t>(total_bits);
        uint32_t tf_tell = enc.tell();
        int logp = 4;  // not transient
        uint32_t tf_select_rsv = lm > 0 && tf_tell + logp + 1 <= budget;
        budget -= tf_select_rsv;
        for (int i = start; i < end; i++) {
            if (tf_tell + logp <= budget) {
                enc.enc_bit_logp(0, static_cast<unsigned>(logp));
                tf_tell = enc.tell();
            }
            logp = 5;
        }
        // kTfSelectTable[lm][0] == kTfSelectTable[lm][2] == 0: no select
        // bit for the all-zero, non-transient case.
    }

    tell = static_cast<int32_t>(enc.tell());
    int spread = kSpreadNormal;
    if (tell + 4 <= total_bits) enc.enc_icdf(spread, celt::kSpreadIcdf, 5);

    int cap[kNbEBands];
    init_caps(cap, lm, C);

    // Dynalloc: no boosts — write a single zero flag per band wherever
    // the decoder would look for one.
    int offsets[kNbEBands] = { 0 };
    {
        int dynalloc_logp = 6;
        int32_t total_bits_q3 = total_bits << kBitRes;
        int32_t tellf = static_cast<int32_t>(enc.tell_frac());
        for (int i = start; i < end; i++) {
            if (tellf + (dynalloc_logp << kBitRes) < total_bits_q3 &&
                0 < cap[i]) {
                enc.enc_bit_logp(0, static_cast<unsigned>(dynalloc_logp));
                tellf = static_cast<int32_t>(enc.tell_frac());
            }
        }
    }

    int alloc_trim = 5;
    if (static_cast<int32_t>(enc.tell_frac()) + (6 << kBitRes) <=
        (total_bits << kBitRes))
        enc.enc_icdf(alloc_trim, celt::kTrimIcdf, 7);

    // No transient => no anti-collapse reservation.
    int32_t bits = ((static_cast<int32_t>(nbytes) * 8) << kBitRes) -
                   static_cast<int32_t>(enc.tell_frac()) - 1;

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
    int tf_res[kNbEBands] = { 0 };
    uint32_t seed = 0;
    quant_all_bands_enc(start, end, x_norm, C == 2 ? x_norm + n : nullptr,
                        collapse_masks, band_e, pulses, /*short_blocks=*/0,
                        spread, dual_stereo, intensity, tf_res,
                        (static_cast<int32_t>(nbytes) * 8) << kBitRes,
                        balance, enc, lm, coded_bands, &seed);

    quant_energy_finalise(start, end, old_ebands_, energy_error_,
                          fine_quant, fine_priority,
                          nbytes * 8 - static_cast<int>(enc.tell()), enc,
                          C);

    final_range_ = enc.range();
    enc.done();
    if (enc.error()) return -2;
    (void)imax;
    (void)imin;
    return nbytes;
}

}  // namespace opus
}  // namespace glint
