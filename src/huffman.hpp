// glint - Huffman encoding
// MIT License - Clean-room implementation

#ifndef GLINT_HUFFMAN_HPP
#define GLINT_HUFFMAN_HPP

#include <cstdint>
#include "bitstream.hpp"
#include "tables.hpp"

namespace glint {

// Region info for Huffman encoding
struct HuffRegions {
    int big_values;      // number of pairs in big-values region (0..288)
    int count1;          // number of quads in count1 region
    int rzero;           // index where trailing zeros begin
    int region0_count;   // scalefactor bands in region 0
    int region1_count;   // scalefactor bands in region 1
    int table_select[3]; // Huffman table for each big-values sub-region
    int count1table;     // 0 = table A (32), 1 = table B (33)
};

// Count bits needed to encode the quantized spectrum (dry run)
// ix: quantized spectrum (576 values, unsigned magnitudes)
// signs: sign flags (true = negative) for each value
// regions: region info
// Returns total bits needed
int huffman_count_bits(const int* ix, const HuffRegions& regions,
                       int sr_index);

// Encode the quantized spectrum into the bitstream
// ix: quantized spectrum (576 values, signed)
// regions: region info
// bs: bitstream writer to output to
void huffman_encode(const int* ix, const HuffRegions& regions,
                    int sr_index, BitstreamWriter& bs);

// Determine regions from quantized spectrum
// ix: quantized spectrum (576 values, unsigned magnitudes)
// sr_index: sample rate index (for scalefactor band boundaries)
// Returns filled HuffRegions struct
HuffRegions huffman_determine_regions(const int* ix, int sr_index);

// Select the best Huffman table for a region
// Returns table_id that minimizes bit count
int select_best_table(const int* ix, int start, int end);

} // namespace glint

#endif // GLINT_HUFFMAN_HPP
