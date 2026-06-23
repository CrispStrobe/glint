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
    // ISO 11172-3: X[i] = s[31-i]
    for (int i = 0; i < 32; i++)
        window_buf_[(window_offset_ + 31 - i) & 0x1FF] = samples[i];

    double z[64];
    for (int j = 0; j < 64; j++) {
        double sum = 0.0;
        for (int p = 0; p < 8; p++) {
            int buf_idx = (window_offset_ + j + 64 * p) & 0x1FF;
            sum += window_buf_[buf_idx] * tables::analysis_window_d[j + 64 * p];
        }
        z[j] = sum;
    }

    for (int i = 0; i < 32; i++) {
        double sum = 0.0;
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
//   PCM (int16) -> Q31 via << 16
//   Window buffer: int32_t Q31
//   Windowed sum z[]: int64_t Q61 (full precision, 8 Q31*Q30 products)
//   Matrixing: z_Q61 * matrix_Q31 = __int128 Q92 accumulator over 64 terms
//     Output: (sum >> 68) gives Q24 (range [-128, 128))

SubbandAnalysisFP::SubbandAnalysisFP() { reset(); }

void SubbandAnalysisFP::reset() {
    std::memset(window_buf_, 0, sizeof(window_buf_));
    window_offset_ = 0;
}

void SubbandAnalysisFP::process_slot(const int16_t* samples, int32_t subband_out[kNumSubbands]) {
    window_offset_ = (window_offset_ - 32) & 0x1FF;
    for (int i = 0; i < 32; i++)
        window_buf_[(window_offset_ + 31 - i) & 0x1FF] = pcm_to_q31(samples[i]);

    int64_t z[64];
    for (int j = 0; j < 64; j++) {
        int64_t sum = 0;
        for (int p = 0; p < 8; p++) {
            int buf_idx = (window_offset_ + j + 64 * p) & 0x1FF;
            sum += static_cast<int64_t>(window_buf_[buf_idx]) * tables::analysis_window[j + 64 * p];
        }
        z[j] = sum;
    }

    for (int i = 0; i < 32; i++) {
        __int128 sum = 0;
        for (int k = 0; k < 64; k++)
            sum += static_cast<__int128>(z[k]) * tables::subband_matrix[i][k];
        subband_out[i] = static_cast<int32_t>(static_cast<int64_t>(sum >> 68));
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
