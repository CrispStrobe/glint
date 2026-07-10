// Opus packet decoding (TOC + framing per RFC 6716 section 3) — CELT-only
// MIT License - Clean-room implementation
//
// Parses the TOC byte and frame packing codes 0-3 (incl. padding and VBR),
// then decodes each frame with the CELT decoder. Scope (PLAN § O1): CELT-
// only configurations (TOC configs 16..31) at 48 kHz output, fullband;
// SILK and hybrid configs are rejected until O2.

#pragma once

#include <cstdint>

#include "opus_celt_decoder.hpp"
#include "opus_silk_decoder.hpp"

namespace glint {
namespace opus {

// Soft clipper of the reference int16 decode API: squashes overshoot beyond
// ±1 with a per-excursion parabola (zero-crossing to zero-crossing), carrying
// the nonlinearity across frames via declip_mem[channels]. Float arithmetic
// on purpose — matches the reference exactly where it shapes output samples.
void pcm_soft_clip(float* x, int frames, int channels, float* declip_mem);

struct OpusPacket {
    int config = 0;        // TOC config (0..31)
    int stereo = 0;
    int frame_count = 0;
    int frame_size = 0;    // samples per frame at 48 kHz
    const uint8_t* frames[48];
    int16_t sizes[48];
};

// Parse TOC + framing. Returns 0 or a negative error.
int opus_packet_parse(const uint8_t* data, int32_t len, OpusPacket* pkt);

class OpusDecoder {
public:
    // channels: 1 or 2 (must match the streams fed in for now).
    void init(int channels) {
        channels_ = channels;
        celt_.init(channels);
        silk_ = silk::SilkDecoder();
        prev_mode_ = 0;
        final_range_ = 0;
    }

    // Decode one packet into interleaved float PCM (±1.0). Returns the
    // number of samples per channel, or a negative error. max_samples
    // guards the output buffer (per channel).
    int decode(const uint8_t* data, int32_t len, float* pcm,
               int max_samples);

    // Conceal a LOST packet of frame_size samples per channel. data (the
    // packet FOLLOWING the loss) may carry SILK in-band FEC (LBRR): its
    // redundant copy covers the last packet-duration of the gap and PLC
    // fills any remainder before it. data == nullptr (or no usable FEC:
    // CELT-only packet/mode, or frame_size shorter than the packet
    // duration) falls back to plain PLC for the whole frame_size
    // (reference opus_decode(..., decode_fec=1)).
    int decode_fec(const uint8_t* data, int32_t len, float* pcm,
                   int frame_size);

    // Range-coder state after the last decoded frame (the Opus
    // conformance "final range" value).
    uint32_t final_range() const { return final_range_; }

private:
    // data == nullptr runs concealment (lost packet / DTX / transition
    // fade source) in the previous mode. fec decodes the packet's SILK
    // LBRR copies (CELT band gets loss concealment).
    int decode_frame_impl(const uint8_t* data, int32_t size, float* pcm,
                          int frame_size, int config, int stereo_flag,
                          int fec = 0);

    int channels_ = 0;
    uint32_t final_range_ = 0;
    int prev_mode_ = 0;  // 1 SILK, 2 hybrid, 3 CELT; 0 = none yet
    int prev_redundancy_ = 0;
    CeltDecoder celt_;
    silk::SilkDecoder silk_;
};

}  // namespace opus
}  // namespace glint
