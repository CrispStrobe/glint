// Minimal Opus decoder CLI over glint's clean-room decoder (PLAN § O1).
// Reads opus_demo's .bit format (per packet: 4-byte BE length, 4-byte BE
// encoder final range, payload), decodes CELT-only packets, writes
// interleaved int16 PCM, and verifies our decoder's final range against
// the encoder's — the Opus conformance check — for every packet.
//
// usage: opus_dec_cli <in.bit> <channels> <out.pcm>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "opus_decoder.hpp"

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: %s in.bit channels out.pcm\n", argv[0]);
        return 2;
    }
    FILE* in = std::fopen(argv[1], "rb");
    if (!in) return 2;
    int channels = std::atoi(argv[2]);
    FILE* out = std::fopen(argv[3], "wb");
    if (!out) return 2;

    glint::opus::OpusDecoder dec;
    dec.init(channels);

    std::vector<float> pcm(2 * 5760);
    std::vector<int16_t> pcm16(2 * 5760);
    float declip_mem[2] = { 0, 0 };
    uint8_t hdr[8], payload[2048];
    int packet = 0, range_mismatches = 0;
    while (std::fread(hdr, 1, 8, in) == 8) {
        uint32_t len = be32(hdr);
        uint32_t enc_range = be32(hdr + 4);
        if (len > sizeof(payload)) return 3;
        if (std::fread(payload, 1, len, in) != len) return 3;
        int n = dec.decode(payload, (int32_t)len, pcm.data(), 5760);
        if (n < 0) {
            std::fprintf(stderr, "packet %d: decode error %d\n", packet, n);
            return 4;
        }
        if (dec.final_range() != enc_range) {
            std::fprintf(stderr,
                         "packet %d: final range mismatch (dec %08x enc "
                         "%08x)\n",
                         packet, dec.final_range(), enc_range);
            range_mismatches++;
        }
        // Match the reference int16 API: soft clip, then round.
        glint::opus::pcm_soft_clip(pcm.data(), n, channels, declip_mem);
        for (int i = 0; i < n * channels; i++) {
            double v = std::lrint(pcm[i] * 32768.0);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            pcm16[i] = (int16_t)v;
        }
        std::fwrite(pcm16.data(), sizeof(int16_t), n * channels, out);
        packet++;
    }
    std::fclose(in);
    std::fclose(out);
    std::printf("%d packets, %d range mismatches\n", packet,
                range_mismatches);
    return range_mismatches ? 1 : 0;
}
