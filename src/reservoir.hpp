// glint - Bit reservoir
// MIT License - Clean-room implementation

#ifndef GLINT_RESERVOIR_HPP
#define GLINT_RESERVOIR_HPP

#include <cstdint>
#include <algorithm>

namespace glint {

class BitReservoir {
public:
    BitReservoir() { reset(); }

    void reset() {
        reservoir_size_ = 0;
    }

    void init(int mean_bits_per_frame) {
        mean_bits_ = mean_bits_per_frame;
        // Max reservoir per MPEG-1: 7680 bits
        reservoir_max_ = 7680;
        reservoir_size_ = 0;
    }

    // Get available bits for current frame
    int get_available_bits() const {
        return mean_bits_ + std::min(reservoir_size_, reservoir_max_);
    }

    // Update reservoir after encoding a frame
    void update(int used_bits) {
        reservoir_size_ += mean_bits_ - used_bits;
        if (reservoir_size_ < 0) reservoir_size_ = 0;
        if (reservoir_size_ > reservoir_max_) reservoir_size_ = reservoir_max_;
    }

    // Get main_data_begin value (in bytes)
    int main_data_begin() const {
        return reservoir_size_ / 8;
    }

    int reservoir_size() const { return reservoir_size_; }
    int mean_bits() const { return mean_bits_; }

private:
    int reservoir_size_;
    int reservoir_max_;
    int mean_bits_;
};

} // namespace glint

#endif // GLINT_RESERVOIR_HPP
