// CELT frame encoder — RFC 6716 section 4.3 (encoder side)
// MIT License - Clean-room implementation
//
// A SIMPLE-first encoder (PLAN § O4): spread normal, no dynalloc boosts,
// neutral trim, no intensity/dual stereo, pure CBR — WITH the pitch
// prefilter (item 1: tonal content) and transient detection + short
// blocks + anti-collapse (item 2: pre-echo). tf_res stays all-zero. Every symbol layer underneath is byte-exact with libopus
// (energy, allocator, theta/PVQ), so streams are valid by construction;
// the top-level DECISIONS are the quality knobs to iterate on later.
//
// Wire spec: the symbol sequence mirrors glint's own conformant decoder
// (opus_celt_decoder.cpp) — silence flag, postfilter bit, transient bit,
// coarse energy (with the intra decision), tf bits, spread, dynalloc,
// trim, allocation, fine energy, band shapes, energy finalise.

#pragma once

#include <cstdint>

#include "opus_ec.hpp"
#include "opus_mdct.hpp"

namespace glint {
namespace opus {

class CeltEncoder {
public:
    void init(int channels);

    // Encode frame_size (120<<LM, LM 0..3) samples per channel of float
    // PCM (±1.0, interleaved) into exactly nbytes (CBR). Returns nbytes,
    // or a negative error. final_range() afterwards gives the range-coder
    // state for conformance checks.
    int encode_frame(const float* pcm, int frame_size, uint8_t* out,
                     int nbytes);

    uint32_t final_range() const { return final_range_; }

private:
    static constexpr int kOverlap = 120;
    static constexpr int kMaxFrame = 960;

    static constexpr int kCombMaxPeriod = 1024;

    int channels_ = 0;
    uint32_t final_range_ = 0;
    float preemph_mem_[2] = {};
    // FILTERED overlap history per channel (MDCT input head).
    double in_mem_[2][kOverlap] = {};
    // UNFILTERED pre-emphasized input history (prefilter comb source).
    double prefilter_mem_[2][kCombMaxPeriod] = {};
    int prefilter_period_ = 0;
    double prefilter_gain_ = 0;
    int prefilter_tapset_ = 0;
    int consec_transient_ = 0;
    // Spread/tapset analysis state (recursive averages + hysteresis).
    int tonal_average_ = 256;
    int hf_average_ = 0;
    int tapset_decision_ = 0;
    int spread_decision_ = 2;  // SPREAD_NORMAL
    float old_ebands_[2 * 21] = {};
    float energy_error_[2 * 21] = {};
    float delayed_intra_ = 1.0f;
    int last_coded_bands_ = 0;
    CeltImdct mdct_;
    double window_[kOverlap];
};

}  // namespace opus
}  // namespace glint
