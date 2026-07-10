// glint CELT-only Opus ENCODER CLI (PLAN § O4): raw int16 PCM @48k ->
// opus_demo .bit format (4B BE length, 4B BE final range, TOC+payload).
// opus_demo -d then VERIFIES our final ranges against libopus's decoder —
// the conformance identity, checked by the reference implementation.
//
// usage: opus_enc_cli <in.raw> <channels> <bitrate_bps> <frame_ms x10:
//        25|50|100|200> <out.bit>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "opus_celt_encoder.hpp"

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr,
                     "usage: %s in.raw channels bitrate frame_ms_x10 "
                     "out.bit\n",
                     argv[0]);
        return 2;
    }
    FILE* in = std::fopen(argv[1], "rb");
    if (!in) return 2;
    int channels = std::atoi(argv[2]);
    int bitrate = std::atoi(argv[3]);
    int ms_x10 = std::atoi(argv[4]);
    int frame;
    int config;
    switch (ms_x10) {
    case 25: frame = 120; config = 28; break;
    case 50: frame = 240; config = 29; break;
    case 100: frame = 480; config = 30; break;
    case 200: frame = 960; config = 31; break;
    default: return 2;
    }
    FILE* out = std::fopen(argv[5], "wb");
    if (!out) return 2;

    // CBR bytes per frame (excluding the TOC byte).
    int nbytes = bitrate * frame / 48000 / 8 - 1;
    if (nbytes < 2) nbytes = 2;
    if (nbytes > 1275) nbytes = 1275;

    glint::opus::CeltEncoder enc;
    enc.init(channels);

    std::vector<int16_t> pcm16(frame * channels);
    std::vector<float> pcm(frame * channels);
    std::vector<uint8_t> pkt(1 + nbytes);
    int frames = 0;
    for (;;) {
        size_t got = std::fread(pcm16.data(), sizeof(int16_t),
                                frame * channels, in);
        if (got < static_cast<size_t>(frame * channels)) break;
        for (int i = 0; i < frame * channels; i++)
            pcm[i] = pcm16[i] * (1.0f / 32768.0f);
        pkt[0] = static_cast<uint8_t>((config << 3) |
                                      ((channels == 2) << 2) | 0);
        int ret = enc.encode_frame(pcm.data(), frame, pkt.data() + 1,
                                   nbytes);
        if (ret < 0) {
            std::fprintf(stderr, "frame %d: encode error %d\n", frames,
                         ret);
            return 3;
        }
        uint32_t len = static_cast<uint32_t>(1 + nbytes);
        uint32_t rng = enc.final_range();
        uint8_t hdr[8] = {
            static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16),
            static_cast<uint8_t>(len >> 8),  static_cast<uint8_t>(len),
            static_cast<uint8_t>(rng >> 24), static_cast<uint8_t>(rng >> 16),
            static_cast<uint8_t>(rng >> 8),  static_cast<uint8_t>(rng),
        };
        std::fwrite(hdr, 1, 8, out);
        std::fwrite(pkt.data(), 1, len, out);
        frames++;
    }
    std::fclose(in);
    std::fclose(out);
    std::printf("encoded %d frames, %d bytes/frame\n", frames, nbytes + 1);
    return 0;
}
