// Cross-check driver for the top-level SILK decoder (PLAN § O2).
// Compiled twice by tools/crosscheck_opus_silk_dec.py. Fuzz oracle at the
// silk_Decode level: header flags, LBRR skip, stereo glue, per-channel
// frames, unmix, resampling to 48 kHz — byte-identical PCM + tells.
// Sequences chain packets and include mono<->stereo stream transitions.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef USE_LIBOPUS
extern "C" {
#include "main.h"
#include "API.h"
#include "entdec.h"
}
struct SideA {
    std::vector<uint8_t> st;
    ec_dec d;
    void init() {
        opus_int32 sz = 0;
        silk_Get_Decoder_Size(&sz);
        st.assign(sz, 0);
        silk_InitDecoder(st.data());
    }
    void stream(uint8_t* b, uint32_t n) { ec_dec_init(&d, b, n); }
    int frame(int16_t* out, int ch_api, int ch_int, int khz, int ms,
              int new_packet) {
        silk_DecControlStruct ctl;
        memset(&ctl, 0, sizeof(ctl));
        ctl.nChannelsAPI = ch_api;
        ctl.nChannelsInternal = ch_int;
        ctl.API_sampleRate = 48000;
        ctl.internalSampleRate = khz * 1000;
        ctl.payloadSize_ms = ms;
        opus_int32 n_out = 0;
        int ret = silk_Decode(st.data(), &ctl, FLAG_DECODE_NORMAL,
                              new_packet, &d, out, &n_out, 0);
        return ret ? -1000 - ret : (int)n_out;
    }
    uint32_t tell() { return (uint32_t)ec_tell(&d); }
};
#else
#include "opus_silk_decoder.hpp"
struct SideA {
    glint::opus::silk::SilkDecoder dec;
    glint::opus::RangeDecoder d;
    void init() { dec = glint::opus::silk::SilkDecoder(); }
    void stream(const uint8_t* b, uint32_t n) { d.init(b, n); }
    int frame(int16_t* out, int ch_api, int ch_int, int khz, int ms,
              int new_packet) {
        return dec.decode(d, out, ch_api, ch_int, khz, 48000, ms,
                          new_packet != 0);
    }
    uint32_t tell() { return d.tell(); }
};
#endif

static uint32_t rng_state;
static uint32_t xrand() {
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return rng_state = x;
}

int main() {
    static uint8_t buf[4096];
    static int16_t out[2 * 960 * 3];
    static const int kFs[3] = { 8, 12, 16 };
    for (uint32_t seed = 1; seed <= 150; seed++) {
        rng_state = seed;
        SideA a;
        a.init();
        std::printf("seed %u\n", seed);
        // Several packets; channel count / rate / duration can change at
        // packet boundaries (as TOC changes would cause).
        for (int p = 0; p < 3; p++) {
            int khz = kFs[xrand() % 3];
            int ch_int = 1 + (int)(xrand() & 1);
            int ch_api = 1 + (int)(xrand() & 1);
            int ms;
            switch (xrand() % 4) {
            case 0: ms = 10; break;
            case 1: ms = 20; break;
            case 2: ms = 40; break;
            default: ms = 60; break;
            }
            int frames = ms <= 20 ? 1 : ms / 20;
            uint32_t len = 40 + xrand() % 2000;
            for (uint32_t i = 0; i < len; i++) buf[i] = (uint8_t)xrand();
            a.stream(buf, len);
            std::printf("pkt %d khz %d chi %d cha %d ms %d len %u\n", p,
                        khz, ch_int, ch_api, ms, len);
            for (int f = 0; f < frames; f++) {
                int n = a.frame(out, ch_api, ch_int, khz, ms, f == 0);
                std::printf("f %d n %d tell %u pcm", f, n, a.tell());
                if (n > 0)
                    for (int i = 0; i < n * ch_api; i += 5)
                        std::printf(" %d", out[i]);
                std::printf("\n");
            }
        }
    }
    return 0;
}
