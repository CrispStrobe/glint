// glint - Quantization loop
// MIT License - Clean-room implementation

#ifndef GLINT_QUANTIZE_HPP
#define GLINT_QUANTIZE_HPP

#include <cstdint>
#include "huffman.hpp"

namespace glint {

struct GranuleInfo {
    int16_t ix[576];
    int global_gain;
    int scalefac[21];
    int subblock_gain[3];  // per-window gain relief (block_type 2 only)
    int scalefac_s[12][3]; // short-window scalefactors (block_type 2 only):
                           // 12 transmitted short sfbs x 3 windows (band 12,
                           // the short sfb21 analog, has no scalefactor)
    int scalefac_compress;
    int part2_3_length;
    int part2_length;
    HuffRegions regions;
    int preflag;
    int scalefac_scale;
    int block_type;  // 0 = long (normal), 2 = short
};

// Set the worker-thread count for the per-granule scale-factor search.
// 1 = single-threaded (default). The search reduces candidates in a fixed
// index order, so the result is byte-identical for any thread count.
void quantize_set_threads(int n);

// gain_floor > 0 keeps the gain search from quantizing finer than that gain
// (the CBR rate controller's constant-quality anchor; savings bank in the
// bit reservoir). allow_psy gates the NMR outer loop: it must be false for
// the SIDE channel of an M/S pair — side-channel noise leaks into decoded
// L/R where the side signal does not mask it, and shaping against the side
// channel's own masks was measured to destroy joint-mode quality
// (36.98 -> 16.47 dB SNR on speech).
// block_type: 0 long, 1 start, 2 short (reordered 3x12 spectrum), 3 stop.
// Types 1/2/3 use the window-switching side-info/region layout; type 2
// additionally gets per-window subblock_gain chosen from window peaks.
GranuleInfo quantize_granule(const double* mdct_in, int available_bits,
                              int sr_index, int quality_mode = 0,
                              int block_type = 0, int gain_floor = 0,
                              bool allow_psy = true);

// VBR quantization: starts from a fixed target gain (vbr_quality 0=best to
// 9=worst) for variable quality, but never exceeds available_bits per
// granule. With the bit reservoir disabled every frame must be
// self-contained, so a loud granule at a fine target gain is coarsened until
// it fits its share of the frame; otherwise the frame's main data overflows
// and the decoder desyncs ("invalid backstep").
GranuleInfo quantize_granule_vbr(const double* mdct_in, int available_bits,
                                  int sr_index, int quality_mode, int vbr_quality,
                                  int block_type = 0);

} // namespace glint

#endif // GLINT_QUANTIZE_HPP
