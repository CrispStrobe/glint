// Extract a surround .opus file's layout + packets to a flat format the
// multistream crosscheck driver can read (glint's OggOpusReader is
// independently conformance-tested):
//   u8 channels, u8 streams, u8 coupled, u8 mapping[channels],
//   then per packet: u32 BE length + payload.
//
// usage: opus_ms_extract <in.opus> <out.pkts>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "opus_ogg.hpp"

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    FILE* in = std::fopen(argv[1], "rb");
    if (!in) return 2;
    std::fseek(in, 0, SEEK_END);
    long n = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    if (std::fread(buf.data(), 1, buf.size(), in) != buf.size()) return 2;
    std::fclose(in);

    glint::opus::OggOpusReader r;
    int err = r.parse(buf.data(), buf.size());
    if (err) {
        std::fprintf(stderr, "parse error %d\n", err);
        return 3;
    }
    const auto& h = r.head();
    FILE* out = std::fopen(argv[2], "wb");
    if (!out) return 2;
    uint8_t hdr[3] = { static_cast<uint8_t>(h.channels),
                       static_cast<uint8_t>(h.stream_count),
                       static_cast<uint8_t>(h.coupled_count) };
    std::fwrite(hdr, 1, 3, out);
    std::fwrite(h.mapping, 1, static_cast<size_t>(h.channels), out);
    for (int i = 0; i < r.packet_count(); i++) {
        const auto& p = r.packet(i);
        uint32_t len = static_cast<uint32_t>(p.size());
        uint8_t l[4] = { static_cast<uint8_t>(len >> 24),
                         static_cast<uint8_t>(len >> 16),
                         static_cast<uint8_t>(len >> 8),
                         static_cast<uint8_t>(len) };
        std::fwrite(l, 1, 4, out);
        std::fwrite(p.data(), 1, p.size(), out);
    }
    std::fclose(out);
    std::printf("channels=%d streams=%d coupled=%d packets=%d\n",
                h.channels, h.stream_count, h.coupled_count,
                r.packet_count());
    return 0;
}
