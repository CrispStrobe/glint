// Mux an opus_demo .bit stream into an Ogg .opus file with glint's
// OggOpusWriter (PLAN § O4 — the encoder's output container).
//
// usage: opusfile_mux_cli <in.bit> <out.opus> [pre_skip]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "opus_decoder.hpp"
#include "opus_ogg.hpp"

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s in.bit out.opus [pre_skip]\n",
                     argv[0]);
        return 2;
    }
    FILE* in = std::fopen(argv[1], "rb");
    if (!in) return 2;
    int pre_skip = argc > 3 ? std::atoi(argv[3]) : 0;

    glint::opus::OggOpusWriter w;
    uint8_t hdr[8], payload[2048];
    bool begun = false;
    int packets = 0;
    while (std::fread(hdr, 1, 8, in) == 8) {
        uint32_t len = be32(hdr);
        if (len > sizeof(payload)) return 3;
        if (std::fread(payload, 1, len, in) != len) return 3;
        glint::opus::OpusPacket pkt;
        if (glint::opus::opus_packet_parse(payload, (int32_t)len, &pkt))
            return 3;
        if (!begun) {
            w.begin(pkt.stereo ? 2 : 1, pre_skip, 48000);
            begun = true;
        }
        w.add_packet(payload, len, pkt.frame_count * pkt.frame_size);
        packets++;
    }
    std::fclose(in);
    if (!begun) return 3;
    const auto& bytes = w.finish();
    FILE* out = std::fopen(argv[2], "wb");
    if (!out) return 2;
    std::fwrite(bytes.data(), 1, bytes.size(), out);
    std::fclose(out);
    std::printf("muxed %d packets, %zu bytes\n", packets, bytes.size());
    return 0;
}
