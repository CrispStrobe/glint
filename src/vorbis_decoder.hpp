// glint - Ogg-Vorbis I decoder
// MIT License - Clean-room implementation
//
// Decodes a complete in-memory Ogg-Vorbis I logical stream to interleaved
// float PCM. Implemented from the Vorbis I specification (xiph.org) only;
// no third-party codec source consulted. See PLAN.md "# Vorbis track".
//
// Scope: the three Vorbis header packets (identification / comment / setup),
// codebooks (scalar + VQ lookup types 1/2), floor types 0 (LSP) and 1
// (piecewise linear), residue types 0/1/2 with inverse channel coupling,
// mapping type 0, the inverse MDCT and windowed overlap-add. Single logical
// stream, mono/stereo and beyond (up to the channel count .sf3 needs).

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace glint {
namespace vorbis {

// Decode a whole Ogg-Vorbis buffer to interleaved float PCM (+-1.0).
// Returns 0 on success and fills `pcm` (interleaved), `sample_rate`,
// `channels`. Negative on error / not a Vorbis stream.
int decode_ogg(const uint8_t* ogg, size_t len, std::vector<float>& pcm,
               int& sample_rate, int& channels);

// ---- Internals exposed for unit tests -------------------------------------

// A decoded Vorbis codebook (spec §3).
struct Codebook {
    int dimensions = 0;
    int entries = 0;
    std::vector<uint8_t> lengths;     // codeword length per entry (0 = unused)
    // Huffman decode: flat tables built from `lengths` (canonical, per spec).
    std::vector<int32_t> fast;        // direct LUT for short codes
    int fast_bits = 0;
    // Fallback association for longer codes: (code,len)->entry sorted list.
    std::vector<uint32_t> codes;      // assigned codeword per entry
    // VQ lookup (spec §3.2).
    int lookup_type = 0;
    float minimum_value = 0.f;
    float delta_value = 0.f;
    int value_bits = 0;
    int sequence_p = 0;
    std::vector<float> vq;            // entries*dimensions decoded value list

    bool build_huffman();             // assign canonical codewords + LUT
    void build_vq();                  // materialize the VQ value list
    // Decode one entry index from a bit reader (returns -1 on error).
    int decode_scalar(class BitReader& br) const;
    // Decode one VQ vector into out[0..dimensions) (returns -1 on error).
    int decode_vector(class BitReader& br, float* out) const;
};

// Parsed identification header (spec §4.2.2).
struct IdHeader {
    int version = -1;
    int channels = 0;
    uint32_t sample_rate = 0;
    int blocksize0 = 0;  // short block
    int blocksize1 = 0;  // long block
    bool valid = false;
};

// Parse a Vorbis identification header packet (must start with 0x01 "vorbis").
IdHeader parse_id_header(const uint8_t* pkt, size_t len);

}  // namespace vorbis
}  // namespace glint
