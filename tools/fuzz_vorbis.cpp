// Fuzz harness for the Ogg-Vorbis I decoder (whole-buffer path). Feeds
// pure-random buffers, bit-flipped/truncated valid streams, and — the key
// case — MUTATED streams whose Ogg page CRCs are repaired so the mutation
// reaches the setup-header parser (codebooks / floors / residues / mappings),
// the classic Vorbis attack surface (huge codebook dims, bad floor/residue
// params). A correct decoder must never crash, read/write out of bounds,
// over-allocate, or hang. Build with -fsanitize=address,undefined; the
// decode work is bounded (fast iMDCT, capped allocations) so a hang surfaces
// as a wall-clock timeout in the driver.
//
// usage: fuzz_vorbis <valid.ogg> [iters]
// MIT License - Clean-room implementation.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "opus_ogg.hpp"       // glint::opus::ogg_crc
#include "vorbis_decoder.hpp"

static uint32_t g_rng = 0x1234abcdu;
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
    std::vector<uint8_t> b(static_cast<size_t>(n > 0 ? n : 0));
    if (!b.empty() && std::fread(b.data(), 1, b.size(), f) != b.size())
        b.clear();
    std::fclose(f);
    return b;
}

// Recompute every Ogg page CRC in place so a mutated stream passes framing
// and its (malformed) packet payloads reach the Vorbis parsers.
static void repair_crcs(std::vector<uint8_t>& d) {
    size_t off = 0;
    while (off + 27 <= d.size()) {
        if (std::memcmp(d.data() + off, "OggS", 4) != 0) break;
        int nsegs = d[off + 26];
        if (off + 27 + (size_t)nsegs > d.size()) break;
        size_t body = 0;
        for (int i = 0; i < nsegs; i++) body += d[off + 27 + i];
        size_t page_len = 27 + (size_t)nsegs + body;
        if (off + page_len > d.size()) break;
        d[off + 22] = d[off + 23] = d[off + 24] = d[off + 25] = 0;
        uint32_t crc = glint::opus::ogg_crc(d.data() + off, page_len);
        for (int i = 0; i < 4; i++)
            d[off + 22 + i] = static_cast<uint8_t>(crc >> (8 * i));
        off += page_len;
    }
}

static long g_calls = 0;
static void walk(const std::vector<uint8_t>& buf) {
    std::vector<float> pcm;
    int sr = 0, ch = 0;
    glint::vorbis::decode_ogg(buf.data(), buf.size(), pcm, sr, ch);
    g_calls++;
}

int main(int argc, char** argv) {
    std::vector<uint8_t> valid =
        argc > 1 ? readf(argv[1]) : std::vector<uint8_t>();
    int iters = argc > 2 ? std::atoi(argv[2]) : 3000;

    // 1) Pure-random buffers.
    for (int it = 0; it < iters; it++) {
        int len = 8 + static_cast<int>(xr() % 4000);
        std::vector<uint8_t> buf(len);
        for (auto& b : buf) b = static_cast<uint8_t>(xr());
        // Sometimes stamp a plausible Vorbis-ish header prefix.
        if ((xr() & 3) == 0 && buf.size() > 8) {
            std::memcpy(buf.data(), "OggS", 4);
        }
        walk(buf);
    }

    // 2) Bit-flipped + truncated valid streams (mostly rejected at the CRC
    //    check — must still be memory-safe).
    if (!valid.empty()) {
        for (int it = 0; it < iters; it++) {
            std::vector<uint8_t> buf = valid;
            int nf = 1 + static_cast<int>(xr() % 12);
            for (int k = 0; k < nf; k++)
                buf[xr() % buf.size()] ^=
                    static_cast<uint8_t>(1u << (xr() & 7));
            if (xr() & 1) buf.resize(1 + xr() % buf.size());
            walk(buf);
        }
        // 3) CRC-repaired mutations: the mutation reaches the setup parser.
        for (int it = 0; it < iters; it++) {
            std::vector<uint8_t> buf = valid;
            int nf = 1 + static_cast<int>(xr() % 24);
            for (int k = 0; k < nf; k++)
                buf[xr() % buf.size()] ^=
                    static_cast<uint8_t>(1u << (xr() & 7));
            repair_crcs(buf);
            walk(buf);
        }
    }

    std::printf("vorbis: %ld decode calls survived, no crash\n", g_calls);
    return 0;
}
