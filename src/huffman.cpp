// glint - Huffman encoding implementation
// MIT License - Clean-room implementation

#include "huffman.hpp"
#include <cstdlib>
#include <algorithm>

namespace glint {

// Find the best Huffman table for a region via max-value-indexed lookup.
int select_best_table(const int* ix, int start, int end) {
    if (start >= end) return 0;
    int max_val = 0;
    for (int i = start; i < end; i++) {
        int v = std::abs(ix[i]);
        if (v > max_val) max_val = v;
    }
    return tables::choose_huff_table(max_val);
}

HuffRegions huffman_determine_regions(const int* ix, int sr_index) {
    HuffRegions r{};

    // Find rzero: last nonzero value
    int rzero = 576;
    while (rzero > 0 && ix[rzero - 1] == 0) rzero--;

    // Find count1 boundary: region where all values are -1, 0, or 1
    // Scan backwards from rzero, grouping in quads
    int count1_start = rzero;
    // Align to quad boundary
    count1_start = (count1_start + 3) & ~3;
    if (count1_start > rzero) count1_start = rzero;

    // Scan back to find where quads of {-1,0,1} begin
    while (count1_start >= 4) {
        bool all_small = true;
        for (int i = count1_start - 4; i < count1_start; i++) {
            if (i < 576 && std::abs(ix[i]) > 1) {
                all_small = false;
                break;
            }
        }
        if (!all_small) break;
        count1_start -= 4;
    }

    // Ensure count1_start is even (big_values counts pairs)
    if (count1_start & 1) count1_start++;

    r.big_values = count1_start / 2;
    r.count1 = (rzero - count1_start) / 4;
    if (r.count1 < 0) r.count1 = 0;
    r.rzero = rzero;

    // Determine region boundaries using scalefactor bands
    const int* sfb = tables::get_sfb_long_by_unified(sr_index);
    int big_values_end = count1_start;

    // Region 0/1/2 boundaries are at scalefactor band edges
    // region0_count and region1_count specify the number of sfb bands in each region
    // region0 spans sfb bands 0..region0_count
    // region1 spans region0_count+1..region0_count+1+region1_count
    // region2 spans the rest up to big_values

    if (big_values_end == 0) {
        r.region0_count = 0;
        r.region1_count = 0;
        r.table_select[0] = 0;
        r.table_select[1] = 0;
        r.table_select[2] = 0;
    } else {
        // Find which sfb band big_values_end falls in
        int max_band = 0;
        for (int b = 0; b < 22; b++) {
            if (sfb[b] >= big_values_end) { max_band = b; break; }
            if (b == 21) max_band = 22;
        }

        // Divide into up to 3 regions
        // A common split: region0=first 8 bands, region1=next 3 bands, region2=rest
        if (max_band <= 1) {
            r.region0_count = max_band;
            r.region1_count = 0;
        } else if (max_band <= 10) {
            // Put approximately half in region0, rest in region1
            r.region0_count = (max_band + 1) / 2;
            r.region1_count = max_band - r.region0_count - 1;
        } else {
            r.region0_count = 7;  // 8 bands in region 0
            r.region1_count = max_band > 8 ? std::min(max_band - 8, 14) - 1 : 0;
        }

        // Clamp to valid range
        if (r.region0_count > 15) r.region0_count = 15;
        if (r.region1_count > 7) r.region1_count = 7;

        // Compute actual region boundaries in spectrum indices
        int region0_end = sfb[r.region0_count + 1];
        if (region0_end > big_values_end) region0_end = big_values_end;

        int region1_end = sfb[r.region0_count + 1 + r.region1_count + 1];
        if (region1_end > big_values_end) region1_end = big_values_end;

        // Select tables for each region
        r.table_select[0] = select_best_table(ix, 0, region0_end);
        r.table_select[1] = select_best_table(ix, region0_end, region1_end);
        r.table_select[2] = select_best_table(ix, region1_end, big_values_end);
    }

    // Choose count1 table (A=32 vs B=33): try both, pick smaller
    int bits_a = 0, bits_b = 0;
    for (int i = count1_start; i + 3 < rzero; i += 4) {
        bits_a += tables::count1_code_length(32, ix[i], ix[i+1], ix[i+2], ix[i+3]);
        bits_b += tables::count1_code_length(33, ix[i], ix[i+1], ix[i+2], ix[i+3]);
    }
    r.count1table = (bits_b < bits_a) ? 1 : 0;

    return r;
}

int huffman_count_bits(const int* ix, const HuffRegions& regions, int sr_index) {
    const int* sfb = tables::get_sfb_long_by_unified(sr_index);
    int big_values_end = regions.big_values * 2;
    int total = 0;

    // Big values region
    if (big_values_end > 0) {
        int region0_end = sfb[regions.region0_count + 1];
        if (region0_end > big_values_end) region0_end = big_values_end;

        int region1_end = sfb[regions.region0_count + 1 + regions.region1_count + 1];
        if (region1_end > big_values_end) region1_end = big_values_end;

        // Region 0
        for (int i = 0; i < region0_end; i += 2) {
            int y = (i + 1 < region0_end) ? ix[i + 1] : 0;
            total += tables::huff_code_length(regions.table_select[0], ix[i], y);
        }
        // Region 1
        for (int i = region0_end; i < region1_end; i += 2) {
            int y = (i + 1 < region1_end) ? ix[i + 1] : 0;
            total += tables::huff_code_length(regions.table_select[1], ix[i], y);
        }
        // Region 2
        for (int i = region1_end; i < big_values_end; i += 2) {
            int y = (i + 1 < big_values_end) ? ix[i + 1] : 0;
            total += tables::huff_code_length(regions.table_select[2], ix[i], y);
        }
    }

    // Count1 region
    int count1_start = big_values_end;
    int count1_end = count1_start + regions.count1 * 4;
    int ct = regions.count1table ? 33 : 32;
    for (int i = count1_start; i + 3 < count1_end && i + 3 < 576; i += 4) {
        total += tables::count1_code_length(ct, ix[i], ix[i+1], ix[i+2], ix[i+3]);
    }

    return total;
}

// Get the Huffman code for a pair of values, and write to bitstream
static void encode_pair(int table_id, int x, int y, BitstreamWriter& bs) {
    tables::HuffTable ht = tables::get_huff_table(table_id);
    if (table_id == 0) return;

    int ax = std::abs(x);
    int ay = std::abs(y);
    int linbits = ht.linbits;
    int ext_x = 0, ext_y = 0;
    int ext_x_bits = 0, ext_y_bits = 0;

    if (linbits > 0 && ax >= 15) {
        ext_x = ax - 15;
        ext_x_bits = linbits;
        ax = 15;
    }
    if (linbits > 0 && ay >= 15) {
        ext_y = ay - 15;
        ext_y_bits = linbits;
        ay = 15;
    }

    // Look up the code from the original ISO standard tables
    int idx = ax * ht.xlen + ay;
    int code_len = ht.hlen[idx];
    uint32_t code = 0;
    switch (table_id) {
    case 1:  code = tables::ht1_code[idx]; break;
    case 2:  code = tables::ht2_code[idx]; break;
    case 3:  code = tables::ht3_code[idx]; break;
    case 5:  code = tables::ht5_code[idx]; break;
    case 6:  code = tables::ht6_code[idx]; break;
    case 7:  code = tables::ht7_code[idx]; break;
    case 8:  code = tables::ht8_code[idx]; break;
    case 9:  code = tables::ht9_code[idx]; break;
    case 10: code = tables::ht10_code[idx]; break;
    case 11: code = tables::ht11_code[idx]; break;
    case 12: code = tables::ht12_code[idx]; break;
    case 13: code = tables::ht13_code[idx]; break;
    case 15: code = tables::ht15_code[idx]; break;
    case 16: case 17: case 18: case 19:
    case 20: case 21: case 22: case 23:
        code = tables::ht16_code[idx]; break;
    case 24: case 25: case 26: case 27:
    case 28: case 29: case 30: case 31:
        code = tables::ht24_code[idx]; break;
    default: return;
    }

    bs.write_bits(code, code_len);

    // Write linbits for x if needed
    if (ext_x_bits > 0) {
        bs.write_bits(ext_x, ext_x_bits);
    }
    // Sign bit for x
    if (x != 0) {
        bs.write_bits(x < 0 ? 1 : 0, 1);
    }
    // Write linbits for y if needed
    if (ext_y_bits > 0) {
        bs.write_bits(ext_y, ext_y_bits);
    }
    // Sign bit for y
    if (y != 0) {
        bs.write_bits(y < 0 ? 1 : 0, 1);
    }
}

static void encode_count1(int table_id, int v, int w, int x, int y,
                           BitstreamWriter& bs) {
    int idx = ((v != 0) ? 8 : 0) | ((w != 0) ? 4 : 0) |
              ((x != 0) ? 2 : 0) | ((y != 0) ? 1 : 0);

    const uint8_t* hlen = (table_id == 32) ? tables::ht32_len : tables::ht33_len;

    if (table_id == 33) {
        // Table B: 4-bit codes from ISO table (codes are 15-idx, not idx)
        bs.write_bits(tables::ht33_code[idx], 4);
    } else {
        // Table A (32): use original code table
        bs.write_bits(tables::ht32_code[idx], hlen[idx]);
    }

    // Sign bits for nonzero values
    if (v != 0) bs.write_bits(v < 0 ? 1 : 0, 1);
    if (w != 0) bs.write_bits(w < 0 ? 1 : 0, 1);
    if (x != 0) bs.write_bits(x < 0 ? 1 : 0, 1);
    if (y != 0) bs.write_bits(y < 0 ? 1 : 0, 1);
}

void huffman_encode(const int* ix, const HuffRegions& regions,
                    int sr_index, BitstreamWriter& bs) {
    const int* sfb = tables::get_sfb_long_by_unified(sr_index);
    int big_values_end = regions.big_values * 2;

    if (big_values_end > 0) {
        int region0_end = sfb[regions.region0_count + 1];
        if (region0_end > big_values_end) region0_end = big_values_end;

        int region1_end = sfb[regions.region0_count + 1 + regions.region1_count + 1];
        if (region1_end > big_values_end) region1_end = big_values_end;

        // Region 0
        for (int i = 0; i < region0_end; i += 2) {
            int y = (i + 1 < region0_end) ? ix[i + 1] : 0;
            encode_pair(regions.table_select[0], ix[i], y, bs);
        }
        // Region 1
        for (int i = region0_end; i < region1_end; i += 2) {
            int y = (i + 1 < region1_end) ? ix[i + 1] : 0;
            encode_pair(regions.table_select[1], ix[i], y, bs);
        }
        // Region 2
        for (int i = region1_end; i < big_values_end; i += 2) {
            int y = (i + 1 < big_values_end) ? ix[i + 1] : 0;
            encode_pair(regions.table_select[2], ix[i], y, bs);
        }
    }

    // Count1 region
    int count1_start = big_values_end;
    int count1_end = count1_start + regions.count1 * 4;
    int ct = regions.count1table ? 33 : 32;
    for (int i = count1_start; i + 3 < count1_end && i + 3 < 576; i += 4) {
        encode_count1(ct, ix[i], ix[i+1], ix[i+2], ix[i+3], bs);
    }
}

} // namespace glint
