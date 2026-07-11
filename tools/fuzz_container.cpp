// Fuzz harness for the CONTAINER / whole-file parsers — the untrusted-
// input surface the frame-decoder fuzzer (fuzz_decoders.cpp) does NOT
// reach: the WAV reader (src/wav_io.cpp) and the Ogg-Opus demuxer
// (src/opus_ogg.cpp OggOpusReader::parse). Both size buffers / walk
// segment tables from attacker-controlled header fields — exactly the bug
// class that hides an OOB read or an unbounded alloc. Feeds pure-random
// buffers plus bit-flipped / truncated / extended mutations of synthetic
// valid seeds. Build with -fsanitize=address,undefined; a real parser must
// never crash, read/write OOB, or hang.
//
// usage: fuzz_container [valid.wav] [valid.opus] [iters]

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "opus_ogg.hpp"
#include "wav_io.hpp"

static uint32_t g_rng = 0x243f6a88u;
static uint32_t xr() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static std::vector<uint8_t> readf(const char* p) {
    FILE* f = std::fopen(p, "rb");
    if (!f) return {};
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> b(static_cast<size_t>(n < 0 ? 0 : n));
    if (!b.empty() && std::fread(b.data(), 1, b.size(), f) != b.size())
        b.clear();
    std::fclose(f);
    return b;
}

// A tiny but structurally valid PCM-EXTENSIBLE WAV so mutations exercise
// the fmt / EXTENSIBLE / data parse paths (not just the RIFF reject).
static std::vector<uint8_t> seed_wav() {
    std::vector<uint8_t> w;
    auto s = [&](const char* t) { for (int i = 0; t[i]; i++) w.push_back(t[i]); };
    auto p32 = [&](uint32_t x) {
        w.push_back(x); w.push_back(x >> 8);
        w.push_back(x >> 16); w.push_back(x >> 24);
    };
    auto p16 = [&](uint16_t x) { w.push_back(x); w.push_back(x >> 8); };
    const uint8_t guid[16] = {0x01, 0, 0, 0, 0, 0, 0x10, 0,
                              0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
    s("RIFF"); p32(0); s("WAVE");
    s("fmt "); p32(40);
    p16(0xFFFE); p16(2); p32(44100); p32(176400); p16(4); p16(16);
    p16(22); p16(16); p32(3);  // cbSize, valid_bits, channel_mask
    w.insert(w.end(), guid, guid + 16);
    s("data"); p32(16);
    for (int i = 0; i < 16; i++) w.push_back(static_cast<uint8_t>(i));
    uint32_t sz = static_cast<uint32_t>(w.size() - 8);
    w[4] = sz; w[5] = sz >> 8; w[6] = sz >> 16; w[7] = sz >> 24;
    return w;
}

static void try_wav(const std::vector<uint8_t>& b) {
    std::vector<float> pcm;
    int sr = 0, ch = 0;
    glint::wav_read(b.data(), b.size(), pcm, sr, ch);
}

static void try_ogg(const std::vector<uint8_t>& b) {
    glint::opus::OggOpusReader r;
    r.parse(b.data(), b.size());
}

static void mutate(std::vector<uint8_t> buf, void (*fn)(const std::vector<uint8_t>&)) {
    if (buf.empty()) return;
    int nf = 1 + static_cast<int>(xr() % 12);
    for (int k = 0; k < nf; k++) {
        size_t p = xr() % buf.size();
        buf[p] ^= static_cast<uint8_t>(1u << (xr() & 7));
    }
    uint32_t how = xr() % 3;
    if (how == 1) buf.resize(1 + xr() % buf.size());          // truncate
    else if (how == 2)                                        // extend
        for (int e = xr() % 64; e > 0; e--) buf.push_back(static_cast<uint8_t>(xr()));
    fn(buf);
}

int main(int argc, char** argv) {
    std::vector<uint8_t> wav = argc > 1 ? readf(argv[1]) : std::vector<uint8_t>();
    std::vector<uint8_t> opus = argc > 2 ? readf(argv[2]) : std::vector<uint8_t>();
    int iters = argc > 3 ? std::atoi(argv[3]) : 20000;
    if (wav.empty()) wav = seed_wav();

    for (int it = 0; it < iters; it++) {
        // pure-random into both parsers
        int len = 8 + static_cast<int>(xr() % 4000);
        std::vector<uint8_t> rnd(len);
        for (auto& b : rnd) b = static_cast<uint8_t>(xr());
        try_wav(rnd);
        try_ogg(rnd);
        // mutated valid seeds
        mutate(wav, try_wav);
        if (!opus.empty()) mutate(opus, try_ogg);
    }
    std::printf("container: %d iters survived (wav + ogg), no crash\n", iters);
    return 0;
}
