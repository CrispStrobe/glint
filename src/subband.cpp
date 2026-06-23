// glint - Subband analysis filter (double-precision)
// MIT License - Clean-room implementation

#include "subband.hpp"
#include "tables.hpp"
#include <cstring>
#include <cmath>

namespace glint {

SubbandAnalysis::SubbandAnalysis() { reset(); }

void SubbandAnalysis::reset() {
    std::memset(window_buf_, 0, sizeof(window_buf_));
    window_offset_ = 0;
}

void SubbandAnalysis::process_slot(const double* samples, double subband_out[kNumSubbands]) {
    window_offset_ = (window_offset_ - 32) & 0x1FF;
    // ISO 11172-3: X[i] = s[31-i] — store newest sample at buf[offset], oldest at buf[offset+31]
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

} // namespace glint
