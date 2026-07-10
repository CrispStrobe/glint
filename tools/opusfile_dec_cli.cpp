// .opus file decoder CLI over glint's clean-room Opus stack (PLAN § O3):
// Ogg demux -> packet decode -> output gain -> pre-skip / end trim ->
// soft clip -> interleaved int16.
//
// usage: opusfile_dec_cli <in.opus> <out.pcm>   (prints "channels N samples M")

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "opus_decoder.hpp"
#include "opus_ogg.hpp"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s in.opus out.pcm\n", argv[0]);
        return 2;
    }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 2;
    std::fseek(f, 0, SEEK_END);
    long fsize = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(fsize);
    if (std::fread(data.data(), 1, fsize, f) != static_cast<size_t>(fsize))
        return 2;
    std::fclose(f);

    glint::opus::OggOpusReader ogg;
    int err = ogg.parse(data.data(), data.size());
    if (err) {
        std::fprintf(stderr, "ogg parse error %d\n", err);
        return 3;
    }
    const int channels = ogg.head().channels;
    const float gain = static_cast<float>(ogg.output_gain());

    glint::opus::OpusDecoder dec;
    dec.init(channels);

    std::vector<float> pcm;
    std::vector<float> frame(2 * 5760);
    for (int i = 0; i < ogg.packet_count(); i++) {
        const auto& pkt = ogg.packet(i);
        int n = dec.decode(pkt.data(), static_cast<int32_t>(pkt.size()),
                           frame.data(), 5760);
        if (n < 0) {
            std::fprintf(stderr, "packet %d: decode error %d\n", i, n);
            return 4;
        }
        for (int j = 0; j < n * channels; j++)
            pcm.push_back(frame[j] * gain);
    }

    // Edit list: drop pre-skip from the front, clamp to the granule length.
    int64_t total = static_cast<int64_t>(pcm.size()) / channels;
    int64_t start = ogg.head().pre_skip;
    int64_t end = total;
    if (ogg.total_samples() >= 0 &&
        start + ogg.total_samples() < end)
        end = start + ogg.total_samples();
    if (start > end) start = end;

    FILE* out = std::fopen(argv[2], "wb");
    if (!out) return 2;
    float declip_mem[2] = { 0, 0 };
    std::vector<int16_t> block(2 * 5760);
    int64_t pos = start;
    while (pos < end) {
        int n = static_cast<int>(end - pos < 5760 ? end - pos : 5760);
        float* x = pcm.data() + pos * channels;
        glint::opus::pcm_soft_clip(x, n, channels, declip_mem);
        for (int j = 0; j < n * channels; j++) {
            double v = std::lrint(x[j] * 32768.0);
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            block[j] = static_cast<int16_t>(v);
        }
        std::fwrite(block.data(), sizeof(int16_t), n * channels, out);
        pos += n;
    }
    std::fclose(out);
    std::printf("channels %d samples %lld\n", channels,
                static_cast<long long>(end - start));
    return 0;
}
