// Minimal Ogg-Vorbis I decoder CLI over glint's clean-room decoder.
// Reads a whole .ogg file and writes interleaved float32 (f32le) PCM to
// stdout (or a file). First line to stderr: "sr ch frames". Used by the
// decode-vs-reference dB gate (tools/test_vorbis_decoder.py).
//
// usage: vorbis_dec_cli <in.ogg> [out.f32]    (out defaults to stdout)
// MIT License - Clean-room implementation.

#include <cstdint>
#include <cstdio>
#include <vector>

#include "glint/glint.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s in.ogg [out.f32]\n", argv[0]);
        return 2;
    }
    FILE* in = std::fopen(argv[1], "rb");
    if (!in) return 2;
    std::fseek(in, 0, SEEK_END);
    long n = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    std::vector<uint8_t> buf(n > 0 ? n : 0);
    if (n > 0 && std::fread(buf.data(), 1, (size_t)n, in) != (size_t)n) {
        std::fclose(in);
        return 2;
    }
    std::fclose(in);

    int sr = 0, ch = 0, frames = 0;
    float* pcm = glint_vorbis_decode(buf.data(), (int)buf.size(), &sr, &ch,
                                     &frames);
    if (!pcm) {
        std::fprintf(stderr, "decode failed\n");
        return 1;
    }
    std::fprintf(stderr, "%d %d %d\n", sr, ch, frames);

    FILE* out = (argc >= 3) ? std::fopen(argv[2], "wb") : stdout;
    if (!out) {
        glint_free(pcm);
        return 2;
    }
    std::fwrite(pcm, sizeof(float), (size_t)frames * ch, out);
    if (out != stdout) std::fclose(out);
    glint_free(pcm);
    return 0;
}
