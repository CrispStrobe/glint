// glint AAC-LC decoder CLI (PLAN § D2): ADTS .aac -> raw float32 PCM.
// usage: aac_dec_cli <in.aac> <out.f32>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "aac_decoder.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s in.aac out.f32\n", argv[0]);
        return 2;
    }
    FILE* in = std::fopen(argv[1], "rb");
    if (!in) return 2;
    std::fseek(in, 0, SEEK_END);
    long n = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    std::vector<uint8_t> buf(static_cast<size_t>(n));
    if (std::fread(buf.data(), 1, buf.size(), in) != buf.size()) return 2;
    std::fclose(in);

    glint::aac::AacDecoder dec;
    dec.init();
    FILE* out = std::fopen(argv[2], "wb");
    if (!out) return 2;

    std::vector<float> pcm(2 * 1024);
    glint::aac::AacFrameInfo info;
    size_t off = 0;
    int frames = 0, errors = 0;
    while (off + 7 <= buf.size()) {
        if (glint::aac::aac_frame_info(buf.data() + off,
                                       static_cast<int>(buf.size() - off),
                                       &info) < 0) {
            off++;
            continue;
        }
        if (off + static_cast<size_t>(info.frame_bytes) > buf.size())
            break;
        int ret = dec.decode_frame(buf.data() + off,
                                   static_cast<int>(buf.size() - off),
                                   pcm.data(), &info);
        if (ret > 0) {
            std::fwrite(pcm.data(), sizeof(float),
                        static_cast<size_t>(ret) * info.channels, out);
        } else {
            errors++;
        }
        frames++;
        off += static_cast<size_t>(info.frame_bytes);
    }
    std::fclose(out);
    std::printf("%d frames, %d errors, %d Hz, %d ch\n", frames, errors,
                info.sample_rate, info.channels);
    return errors ? 1 : 0;
}
