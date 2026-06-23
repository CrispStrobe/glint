// glint - Quantization loop
// MIT License - Clean-room implementation

#ifndef GLINT_QUANTIZE_HPP
#define GLINT_QUANTIZE_HPP

#include <cstdint>
#include "huffman.hpp"

namespace glint {

struct GranuleInfo {
    int ix[576];
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

GranuleInfo quantize_granule(const double* mdct_in, int available_bits,
                              int sr_index, int quality_mode = 0);

} // namespace glint

#endif // GLINT_QUANTIZE_HPP
