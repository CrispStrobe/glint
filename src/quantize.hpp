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

GranuleInfo quantize_granule(const double* mdct_in, int available_bits,
                              int sr_index, int quality_mode = 0,
                              bool short_block = false);

// VBR quantization: starts from a fixed target gain (vbr_quality 0=best to
// 9=worst) for variable quality, but never exceeds available_bits per
// granule. With the bit reservoir disabled every frame must be
// self-contained, so a loud granule at a fine target gain is coarsened until
// it fits its share of the frame; otherwise the frame's main data overflows
// and the decoder desyncs ("invalid backstep").
GranuleInfo quantize_granule_vbr(const double* mdct_in, int available_bits,
                                  int sr_index, int quality_mode, int vbr_quality,
                                  bool short_block = false);

} // namespace glint

#endif // GLINT_QUANTIZE_HPP
