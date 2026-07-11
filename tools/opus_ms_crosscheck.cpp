// Cross-check driver for multistream (surround) decode — compiled twice
// by tools/crosscheck_opus_ms.py: libopus opus_multistream_decode_float
// vs glint OpusMsDecoder. Input = opus_ms_extract output. Prints per
// packet: index, samples, XOR'd final range, and subsampled PCM scaled
// to int (float APIs both sides, no soft clip).
//
// usage: opus_ms_crosscheck <in.pkts>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#ifdef USE_LIBOPUS
extern "C" {
#include "opus_multistream.h"
}
struct Dec {
    OpusMSDecoder* d = nullptr;
    int ch = 0;
    void init(int channels, int streams, int coupled,
              const uint8_t* mapping) {
        int err = 0;
        ch = channels;
        d = opus_multistream_decoder_create(48000, channels, streams,
                                            coupled, mapping, &err);
    }
    int decode(const uint8_t* p, int len, float* pcm) {
        return opus_multistream_decode_float(d, p, len, pcm, 5760, 0);
    }
    uint32_t range() {
        opus_uint32 r = 0;
        opus_multistream_decoder_ctl(d, OPUS_GET_FINAL_RANGE(&r));
        return r;
    }
};
#else
#include "opus_ms_decoder.hpp"
struct Dec {
    glint::opus::OpusMsDecoder d;
    int ch = 0;
    void init(int channels, int streams, int coupled,
              const uint8_t* mapping) {
        ch = channels;
        d.init(channels, streams, coupled, mapping);
    }
    int decode(const uint8_t* p, int len, float* pcm) {
        return d.decode(p, len, pcm, 5760);
    }
    uint32_t range() { return d.final_range(); }
};
#endif

static uint32_t be32(const uint8_t* p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] << 8 | p[3];
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    FILE* in = std::fopen(argv[1], "rb");
    if (!in) return 2;
    uint8_t hdr[3];
    if (std::fread(hdr, 1, 3, in) != 3) return 3;
    int channels = hdr[0], streams = hdr[1], coupled = hdr[2];
    uint8_t mapping[8];
    if (std::fread(mapping, 1, channels, in) !=
        static_cast<size_t>(channels))
        return 3;

    Dec dec;
    dec.init(channels, streams, coupled, mapping);
    std::vector<float> pcm(8 * 5760);

    uint8_t lenb[4];
    std::vector<uint8_t> pkt;
    int i = 0;
    while (std::fread(lenb, 1, 4, in) == 4) {
        uint32_t len = be32(lenb);
        pkt.resize(len);
        if (len && std::fread(pkt.data(), 1, len, in) != len) return 3;
        int n = dec.decode(pkt.data(), static_cast<int>(len), pcm.data());
        std::printf("%d n %d rng %08x pcm", i, n,
                    n >= 0 ? dec.range() : 0);
        for (int k = 0; k < n * channels; k += 173)
            std::printf(" %ld", std::lround(pcm[k] * 32768.0f));
        std::printf("\n");
        i++;
    }
    std::fclose(in);
    return 0;
}
