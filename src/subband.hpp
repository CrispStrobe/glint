// glint - Subband analysis filter (polyphase filter bank)
// MIT License - Clean-room implementation

#ifndef GLINT_SUBBAND_HPP
#define GLINT_SUBBAND_HPP

#include <cstdint>

namespace glint {

static constexpr int kNumSubbands = 32;
static constexpr int kTimeSlots = 36;

class SubbandAnalysis {
public:
    SubbandAnalysis();
    void analyze(const int16_t* pcm, double out[kNumSubbands][kTimeSlots], int num_slots = kTimeSlots);
    void reset();

private:
    double window_buf_[512];
    int window_offset_;
    void process_slot(const double* samples, double subband_out[kNumSubbands]);
};

} // namespace glint

#endif
