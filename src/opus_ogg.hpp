// Ogg Opus container reading — RFC 3533 (Ogg) + RFC 7845 (Opus mapping)
// MIT License - Clean-room implementation
//
// Minimal demuxer for decoding .opus files: page parsing with CRC checks,
// packet reassembly (lacing + cross-page continuation), the OpusHead /
// OpusTags header packets, and the edit-list semantics (pre-skip trimming
// at the front, granule-position trimming at the end, Q7.8 dB output gain).
//
// Scope: single logical stream (the first Opus stream found), channel
// mapping families 0 and 1 with up to 2 channels; multistream/surround is
// deferred (PLAN § O3).

#pragma once

#include <cstdint>
#include <vector>

namespace glint {
namespace opus {

struct OpusHead {
    int version = 0;
    int channels = 0;
    int pre_skip = 0;
    uint32_t input_sample_rate = 0;
    int output_gain_q8 = 0;  // Q7.8 dB, applied by the decoder
    int mapping_family = 0;
};

class OggOpusReader {
public:
    // Parse from a whole file in memory. Returns 0 or a negative error
    // (-1 malformed, -2 not an Opus stream, -3 unsupported mapping).
    int parse(const uint8_t* data, size_t len);

    const OpusHead& head() const { return head_; }
    // Audio packets in order (views into the caller's buffer copied out).
    int packet_count() const { return static_cast<int>(packets_.size()); }
    const std::vector<uint8_t>& packet(int i) const { return packets_[i]; }
    // Total PCM samples per channel at 48 kHz after the edit list
    // (granule of the last page minus pre-skip); -1 if unknown.
    int64_t total_samples() const { return total_samples_; }
    // Linear gain factor from output_gain_q8.
    double output_gain() const;

private:
    OpusHead head_;
    std::vector<std::vector<uint8_t>> packets_;
    int64_t total_samples_ = -1;
};

// Ogg page CRC (poly 0x04C11DB7, no reflection, zero init/xor), exposed
// for tests.
uint32_t ogg_crc(const uint8_t* data, size_t len);

}  // namespace opus
}  // namespace glint
