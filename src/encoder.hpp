// glint - Encoder orchestrator
// MIT License - Clean-room implementation

#ifndef GLINT_ENCODER_HPP
#define GLINT_ENCODER_HPP

#include <cstdint>
#include "glint/glint.h"
#include "subband.hpp"
#include "mdct.hpp"
#include "quantize.hpp"
#include "huffman.hpp"
#include "reservoir.hpp"
#include "bitstream.hpp"
#include "tables.hpp"
#include "psycho.hpp"

// Define at global scope to match the C forward declaration in glint.h
struct glint_context {
    glint_config config;
    int sr_index;
    int br_index;
    int num_channels;
    int mpeg_version;  // 1 = MPEG-1, 0 = MPEG-2, -1 = MPEG-2.5
    int num_granules;  // 2 for MPEG-1, 1 for MPEG-2/2.5
    int frame_size;
    int padding;
    int mean_bits_per_frame;
    int side_info_bits;

    bool use_fixed_point;  // runtime path selection
    int quality_mode;      // 0=speed, 1=normal, 2=best (psychoacoustic masking)

    bool vbr_mode;         // true when VBR encoding is active
    int vbr_quality;       // VBR quality 0-9 (0=best, 9=worst)

#if !defined(GLINT_FIXED_POINT) || defined(GLINT_BOTH_PATHS)
    glint::SubbandAnalysis subband[2];
    glint::MDCT mdct[2];
#endif
#ifdef GLINT_FIXED_POINT
    glint::SubbandAnalysisFP subband_fp[2];
    glint::MDCT_FP mdct_fp[2];
#endif
    // Bit reservoir: continuous main-data stream with deferred frame emission.
    glint::ReservoirStream reservoir;

    // CBR rate controller: constant-quality gain anchor, adapted +-1 per
    // frame from reservoir fill (rc_anchor == 0 means warmup, floor off),
    // plus an EMA (x16 fixed point) of the achieved mean global_gain that
    // bounds the anchor's drift.
    int rc_anchor;
    int rc_gain_ema_x16;

    glint::FrameAssembler frame_asm;
    // Sized to hold several frames: with the reservoir, one encode call can
    // release more than one buffered frame (and flush() drains the tail).
    uint8_t output_buf[glint::kMaxFrameSize * 8];
    int output_size;

    int padding_remainder;
    int padding_threshold;
    int frame_count;

    // Psychoacoustic model (quality_mode >= 2)
    glint::PsychoModel psycho;

    // Transient detection state (per channel)
    double prev_granule_energy[2];  // per channel
    bool prev_energy_valid;

    // Streaming callback (optional)
    glint_write_cb write_cb;
    void* write_cb_data;
};

#endif // GLINT_ENCODER_HPP
