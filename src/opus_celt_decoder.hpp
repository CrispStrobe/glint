// CELT frame decoder — RFC 6716 section 4.3
// MIT License - Clean-room implementation
//
// Decodes one CELT frame (2.5/5/10/20 ms at 48 kHz) given an initialized
// range decoder positioned after any TOC/SILK payload. Covers the full
// synthesis chain: header flags (silence/postfilter/transient/intra),
// coarse energy, tf_res, spread, dynalloc boosts, alloc trim, implicit
// allocation, fine energy, PVQ shapes, anti-collapse, denormalisation,
// inverse MDCT overlap chain, comb-filter postfilter, and de-emphasis.
//
// Scope (PLAN § O1): CELT-only streams, 48 kHz, no downsampling, decoder
// channels == stream channels, no PLC yet (data must be non-NULL).
//
// PCM convention: internal signals use the reference float build's ±32768
// scale; pcm_out() writes interleaved ±1.0 floats.

#pragma once

#include <cstdint>

#include "opus_ec.hpp"
#include "opus_mdct.hpp"

namespace glint {
namespace opus {

class CeltDecoder {
public:
    // channels: 1 or 2. start/end default to the CELT-only full band range.
    void init(int channels);

    // Decode one frame of frame_size samples (120<<LM, LM in 0..3) into
    // interleaved float PCM (±1.0, channels() wide). dec must be freshly
    // initialized on the frame's payload (or positioned after preceding
    // layers). stream_channels may differ from the decoder's channel
    // count (mono streams upmix, stereo streams downmix, like the
    // reference). end_band: 13/17/19/21 for NB/WB/SWB/FB. start_band is 0
    // (CELT-only) or 17 (hybrid: SILK covers the low bands and dec is
    // positioned after the SILK payload; no postfilter, no silence flag).
    // Returns frame_size or a negative error.
    int decode_frame(RangeDecoder& dec, uint32_t payload_bytes, float* pcm,
                     int frame_size, int stream_channels, int end_band,
                     int start_band = 0);

    uint32_t range_final() const { return rng_; }

private:
    static constexpr int kDecodeBufferSize = 2048;
    static constexpr int kOverlap = 120;
    static constexpr int kMaxFrame = 960;

    void synthesis(const double* X, int CC, int C, int is_transient,
                   int lm, int silence, int effend, int start);

    int channels_ = 0;
    uint32_t rng_ = 0;  // LCG seed, re-seeded from the range state per frame
    // Rolling synthesis memory: past output for MDCT overlap + postfilter
    // history (reference DECODE_BUFFER_SIZE + overlap per channel).
    double decode_mem_[2][kDecodeBufferSize + kOverlap];
    double old_ebands_[2 * 21];
    double old_log_e_[2 * 21];
    double old_log_e2_[2 * 21];
    double background_log_e_[2 * 21];
    double preemph_mem_[2];
    int postfilter_period_ = 0;
    int postfilter_period_old_ = 0;
    double postfilter_gain_ = 0;
    double postfilter_gain_old_ = 0;
    int postfilter_tapset_ = 0;
    int postfilter_tapset_old_ = 0;
    CeltImdct imdct_;
    double window_[kOverlap];
};

}  // namespace opus
}  // namespace glint
