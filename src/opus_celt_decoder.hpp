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
// Scope (PLAN § O1): CELT-only streams, 48 kHz, no downsampling. Packet
// loss concealment (decode_lost) covers lost packets and the Opus layer's
// mode-transition fades: pitch-based waveform extrapolation in the
// LPC-excitation domain after two good frames, noise/CNG otherwise.
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

    // Conceal one lost frame (the reference celt_decode_with_ec data==NULL
    // path: celt_decode_lost + de-emphasis). frame_size is 120<<LM like
    // decode_frame; uses the band range of the last decoded frame. After
    // two consecutive good frames the concealment is pitch-based (waveform
    // extrapolation in the LPC excitation domain); at stream start, for
    // hybrid, or after >=100 ms of loss it falls back to per-band noise at
    // decaying energy. Returns frame_size or a negative error.
    int decode_lost(float* pcm, int frame_size);

    uint32_t range_final() const { return rng_; }

private:
    static constexpr int kDecodeBufferSize = 2048;
    static constexpr int kOverlap = 120;
    static constexpr int kMaxFrame = 960;
    static constexpr int kPlcLpcOrder = 24;  // reference CELT_LPC_ORDER

    void synthesis(const double* X, int CC, int C, int is_transient,
                   int lm, int silence, int effend, int start);
    // 1-pole de-emphasis of the last n synthesized samples into ±1.0 PCM.
    void deemphasis(float* pcm, int n);
    // Pre-filter (inverse postfilter) + TDAC fold of the concealed tail so
    // it blends with the next frame's MDCT (first good frame after loss).
    void prefilter_and_fold(int n);
    // Pitch lag over the decode history for the pitch-based PLC.
    int plc_pitch_search() const;

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
    // Loss-concealment state (reference loss_duration / skip_plc /
    // prefilter_and_fold / last_pitch_index / per-channel PLC LPC).
    int loss_duration_ = 0;       // 2.5 ms units since the last good frame
    int skip_plc_ = 0;            // noise PLC until 2 consecutive good frames
    int prefilter_and_fold_ = 0;  // pending fold-in of concealed audio
    int last_pitch_index_ = 0;
    int plc_start_ = 0;           // band range of the last decoded frame
    int plc_end_ = 21;
    double plc_lpc_[2][kPlcLpcOrder];
    CeltImdct imdct_;
    double window_[kOverlap];
};

}  // namespace opus
}  // namespace glint
