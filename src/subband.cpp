// glint - Subband analysis filter
// MIT License - Clean-room implementation

#include "subband.hpp"
#include "tables.hpp"
#include <cstring>
#include <cmath>

#ifdef GLINT_FIXED_POINT
#include "fixedpoint.hpp"
#endif

namespace glint {

// === Double-precision path (always compiled) ===

SubbandAnalysis::SubbandAnalysis() { reset(); }

void SubbandAnalysis::reset() {
    std::memset(window_buf_, 0, sizeof(window_buf_));
    window_offset_ = 0;
}

void SubbandAnalysis::process_slot(const double* samples, double subband_out[kNumSubbands]) {
    window_offset_ = (window_offset_ - 32) & 0x1FF;
    for (int i = 0; i < 32; i++)
        window_buf_[(window_offset_ + 31 - i) & 0x1FF] = samples[i];

    double z[64];
    for (int j = 0; j < 64; j++) {
        double sum = 0.0;

#ifndef _MSC_VER
#pragma GCC unroll 8
#endif
        for (int p = 0; p < 8; p++) {
            int buf_idx = (window_offset_ + j + 64 * p) & 0x1FF;
            sum += window_buf_[buf_idx] * tables::analysis_window_d[j + 64 * p];
        }
        z[j] = sum;
    }

    for (int i = 0; i < 32; i++) {
        double sum = 0.0;

#ifndef _MSC_VER
#pragma GCC unroll 8
#endif
        for (int k = 0; k < 64; k++)
            sum += z[k] * tables::subband_matrix_d[i][k];
        subband_out[i] = sum;
    }
}

void SubbandAnalysis::analyze(const int16_t* pcm, double out[kNumSubbands][kTimeSlots], int num_slots) {
    for (int ts = 0; ts < num_slots; ts++) {
        double samples[32];
        for (int i = 0; i < 32; i++)
            samples[i] = pcm[ts * 32 + i] / 32768.0;
        double slot_out[kNumSubbands];
        process_slot(samples, slot_out);
        for (int sb = 0; sb < kNumSubbands; sb++)
            out[sb][ts] = slot_out[sb];
    }
}

#ifdef GLINT_FIXED_POINT

// === Fixed-point (Q24) path ===
//
// Q format chain:
//   PCM int16 -> int32 Q15 (sign-extend, no shift)
//   Window buffer: int32 Q15
//   Windowed sum: Q15 * Q30 = Q45 in int64. Sum of 8 products.
//   Matrixing: pre-shift z >> 24 to Q21, then Q21 * Q31 = Q52 in int64.
//     Sum of 64 terms fits int64 (worst case ~2.6e18 < 9.2e18).
//   Output: Q52 >> 28 = Q24.

SubbandAnalysisFP::SubbandAnalysisFP() { reset(); }

void SubbandAnalysisFP::reset() {
    std::memset(window_buf_, 0, sizeof(window_buf_));
    window_offset_ = 0;
}

void SubbandAnalysisFP::process_slot(const int16_t* samples, int32_t subband_out[kNumSubbands]) {
    window_offset_ = (window_offset_ - 32) & 0x1FF;
    for (int i = 0; i < 32; i++)
        window_buf_[(window_offset_ + 31 - i) & 0x1FF] = static_cast<int32_t>(samples[i]);

    // Windowed partial sums: Q15 * Q30 = Q45 in int64
    int64_t z[64];
    for (int j = 0; j < 64; j++) {
        int64_t sum = 0;

#ifndef _MSC_VER
#pragma GCC unroll 8
#endif
        for (int p = 0; p < 8; p++) {
            int buf_idx = (window_offset_ + j + 64 * p) & 0x1FF;
            sum += static_cast<int64_t>(window_buf_[buf_idx]) * tables::analysis_window[j + 64 * p];
        }
        z[j] = sum;
    }

    // Matrixing: z[] is Q45, subband_matrix[] is Q31.
    for (int i = 0; i < 32; i++) {
#if defined(__SIZEOF_INT128__)
        // Full precision: Q45 * Q31 = Q76 in __int128. Output: Q76 >> 52 = Q24.
        __int128 sum = 0;

#ifndef _MSC_VER
#pragma GCC unroll 8
#endif
        for (int k = 0; k < 64; k++)
            sum += static_cast<__int128>(z[k]) * tables::subband_matrix[i][k];
        subband_out[i] = static_cast<int32_t>(static_cast<int64_t>(sum >> 52));
#else
        // MSVC fallback: pre-shift z >> 24 to Q21, Q21 * Q31 = Q52 in int64.
        int64_t sum = 0;

#ifndef _MSC_VER
#pragma GCC unroll 8
#endif
        for (int k = 0; k < 64; k++)
            sum += (z[k] >> 24) * static_cast<int64_t>(tables::subband_matrix[i][k]);
        subband_out[i] = static_cast<int32_t>(sum >> 28);
#endif
    }
}

void SubbandAnalysisFP::analyze(const int16_t* pcm, int32_t out[kNumSubbands][kTimeSlots], int num_slots) {
    for (int ts = 0; ts < num_slots; ts++) {
        int32_t slot_out[kNumSubbands];
        process_slot(&pcm[ts * 32], slot_out);
        for (int sb = 0; sb < kNumSubbands; sb++)
            out[sb][ts] = slot_out[sb];
    }
}

#endif // GLINT_FIXED_POINT

} // namespace glint
