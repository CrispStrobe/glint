// glint - AAC-LC quantization, sectioning and noiseless coding
// MIT License - Clean-room implementation

#ifndef GLINT_AAC_CODER_HPP
#define GLINT_AAC_CODER_HPP

#include <cstdint>

namespace glint {
namespace aac {

constexpr int kMaxSfb = 52;       // >= largest kNumSwbLong (51)
constexpr int kMaxQuant = 8191;   // spectral magnitude cap (book 11 escape)
constexpr int kSfOffset = 100;    // scalefactor offset per ISO

// Minimal MSB-first bit writer into a caller-owned buffer.
// count_only mode tallies bits without touching memory (used for rate fitting).
class AacBitWriter {
public:
    AacBitWriter(uint8_t* buf, int capacity)
        : buf_(buf), cap_(capacity) {}
    explicit AacBitWriter(int)  // count-only
        : buf_(nullptr), cap_(0) {}

    void put(uint32_t value, int nbits) {
        bit_count_ += nbits;
        if (!buf_) return;
        while (nbits > 0) {
            int take = nbits > 24 ? 24 : nbits;
            put_raw((value >> (nbits - take)) & ((1u << take) - 1), take);
            nbits -= take;
        }
    }
    void byte_align() {
        int pad = (8 - (bit_count_ & 7)) & 7;
        if (pad) put(0, pad);
    }
    int bits() const { return bit_count_; }
    int bytes() const { return (bit_count_ + 7) / 8; }
    bool overflowed() const { return overflow_; }

private:
    void put_raw(uint32_t v, int n) {
        cache_ = (cache_ << n) | v;
        cache_bits_ += n;
        while (cache_bits_ >= 8) {
            cache_bits_ -= 8;
            if (byte_pos_ < cap_) {
                buf_[byte_pos_++] = static_cast<uint8_t>((cache_ >> cache_bits_) & 0xFF);
            } else {
                overflow_ = true;
            }
        }
    }
    uint8_t* buf_;
    int cap_;
    int byte_pos_ = 0;
    uint64_t cache_ = 0;
    int cache_bits_ = 0;
    int bit_count_ = 0;
    bool overflow_ = false;
};

// One channel's fitted quantization plan for a long-window frame.
struct AacChannelPlan {
    int global_gain;            // = the flat scalefactor for all coded bands
    int max_sfb;
    int num_lines;              // swb_offset[max_sfb]
    uint8_t book[kMaxSfb];      // per-sfb codebook (0 = zero band)
    int16_t ix[1024];           // signed quantized coefficients
    int ics_bits;               // exact individual_channel_stream cost, excl. ics_info
};

// Quantize `spec` (p34 = |spec|^0.75 precomputed) with the flat scalefactor
// `gain`; returns max magnitude. ix gets signed values, magnitudes capped
// implicitly by the caller's gain choice (values > kMaxQuant mean "gain too fine").
int aac_quantize(const double* p34, const double* spec, int n, int gain, int16_t* ix);

// Choose per-band books (optimal sectioning DP) and compute the exact ICS bit
// cost for the given quantized spectrum. Fills plan->book and plan->ics_bits.
void aac_section_and_count(const int16_t* ix, int sr_index, AacChannelPlan* plan);

// Fit a channel: binary-search the flat scalefactor so the exact ICS cost fits
// budget_bits (and magnitudes fit kMaxQuant). spec has 1024 lines; lines at and
// above swb_offset[max_sfb] must already be zero.
void aac_fit_channel(const double* spec, int sr_index, int max_sfb,
                     int budget_bits, AacChannelPlan* plan);

// Emit ics_info / individual_channel_stream. `include_ics_info` follows the
// wire layout: true for SCE (and CPE with common_window=0), false for the two
// ICS of a common_window CPE, whose shared ics_info the caller writes once.
void aac_write_ics_info(AacBitWriter& bw, int max_sfb);
void aac_write_ics_body(AacBitWriter& bw, const AacChannelPlan& plan, int sr_index,
                        bool include_ics_info);

}  // namespace aac
}  // namespace glint

#endif  // GLINT_AAC_CODER_HPP
