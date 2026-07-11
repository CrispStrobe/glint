// glint MP3 decoder CLI (PLAN § D1): .mp3 -> raw float32 interleaved PCM
// on stdout-file. Skips ID3v2, resyncs between frames.
//
// usage: mp3_dec_cli <in.mp3> <out.f32>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "mp3_decoder.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s in.mp3 out.f32\n", argv[0]);
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

    size_t off = 0;
    // ID3v2 skip.
    if (buf.size() > 10 && !std::memcmp(buf.data(), "ID3", 3)) {
        size_t sz = ((buf[6] & 0x7F) << 21) | ((buf[7] & 0x7F) << 14) |
                    ((buf[8] & 0x7F) << 7) | (buf[9] & 0x7F);
        off = 10 + sz;
    }

    glint::mp3::Mp3Decoder dec;
    dec.init();

    // Skip a leading Xing/Info/VBRI metadata frame (valid MP3 frame that
    // players don't render).
    {
        glint::mp3::Mp3FrameInfo fi;
        if (glint::mp3::mp3_frame_info(buf.data() + off,
                                       static_cast<int>(buf.size() - off),
                                       &fi) == 0 &&
            off + static_cast<size_t>(fi.frame_bytes) <= buf.size()) {
            const uint8_t* f = buf.data() + off;
            for (int k = 4; k + 4 <= fi.frame_bytes && k < 44; k++) {
                if (!std::memcmp(f + k, "Xing", 4) ||
                    !std::memcmp(f + k, "Info", 4) ||
                    !std::memcmp(f + k, "VBRI", 4)) {
                    off += static_cast<size_t>(fi.frame_bytes);
                    break;
                }
            }
        }
    }
    FILE* out = std::fopen(argv[2], "wb");
    if (!out) return 2;

    std::vector<float> pcm(2 * 1152);
    glint::mp3::Mp3FrameInfo info;
    int frames = 0, decoded = 0, errors = 0;
    while (off + 4 <= buf.size()) {
        if (glint::mp3::mp3_frame_info(buf.data() + off,
                                       static_cast<int>(buf.size() - off),
                                       &info) < 0) {
            off++;  // resync
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
            decoded++;
        } else if (ret < 0) {
            errors++;
        }
        frames++;
        off += static_cast<size_t>(info.frame_bytes);
    }
    std::fclose(out);
    std::printf("%d frames, %d decoded, %d errors, %d Hz, %d ch\n",
                frames, decoded, errors, info.sample_rate, info.channels);
    return errors ? 1 : 0;
}
