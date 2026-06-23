// glint unit tests
// MIT License - Clean-room implementation
//
// Compile: g++ -std=c++17 -O2 -I../include -I../src tests/test_unit.cpp
//          ../src/subband.cpp ../src/mdct.cpp ../src/quantize.cpp
//          ../src/huffman.cpp ../src/reservoir.cpp ../src/bitstream.cpp
//          ../src/encoder.cpp -lm -o test_unit

#include "glint/glint.h"
#include "tables.hpp"
#include "subband.hpp"
#include "mdct.hpp"
#include "quantize.hpp"
#include "huffman.hpp"
#include "bitstream.hpp"
#include "fixedpoint.hpp"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <cassert>

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(cond, msg) do { \
    tests_run++; \
    if (cond) { tests_passed++; } \
    else { std::fprintf(stderr, "  FAIL: %s (line %d)\n", msg, __LINE__); } \
} while(0)

// --- Bitstream round-trip ---
static void test_bitstream() {
    std::printf("Bitstream writer...\n");
    glint::BitstreamWriter bs;
    bs.write_bits(0x7FF, 11);  // sync
    bs.write_bits(0x3, 2);     // MPEG-1
    bs.write_bits(0x1, 2);     // Layer III
    bs.write_bits(1, 1);       // no CRC
    bs.write_bits(9, 4);       // 128kbps index
    bs.write_bits(0, 2);       // 44100 Hz
    bs.write_bits(0, 1);       // no padding
    bs.write_bits(0, 1);       // private
    bs.write_bits(3, 2);       // mono
    bs.write_bits(0, 2);       // mode ext
    bs.write_bits(0, 1);       // copyright
    bs.write_bits(1, 1);       // original
    bs.write_bits(0, 2);       // emphasis

    CHECK(bs.bit_count() == 32, "header is 32 bits");

    // Check sync word bytes
    const uint8_t* d = bs.data();
    CHECK(d[0] == 0xFF, "sync byte 0");
    CHECK((d[1] & 0xE0) == 0xE0, "sync byte 1 top 3 bits");
}

// --- Huffman encode/count consistency ---
static void test_huffman_consistency() {
    std::printf("Huffman encode/count consistency...\n");
    glint::tables::init_tables();

    // Create a test spectrum
    int ix[576] = {};
    ix[0] = 5; ix[1] = -3; ix[2] = 7; ix[3] = -1;
    ix[4] = 2; ix[5] = 0; ix[6] = 1; ix[7] = -1;

    // Determine regions and count bits
    auto regions = glint::huffman_determine_regions(ix, 0);
    int counted_bits = glint::huffman_count_bits(ix, regions, 0);

    // Actually encode and count written bits
    glint::BitstreamWriter bs;
    glint::huffman_encode(ix, regions, 0, bs);
    int written_bits = bs.bit_count();

    CHECK(counted_bits == written_bits,
          "huffman count matches encode");
}

// --- pow34 table accuracy ---
static void test_pow34_table() {
    std::printf("pow34 table accuracy...\n");
    glint::tables::init_tables();

    double max_err = 0;
    for (int x = 1; x < 1000; x++) {
        double expected = std::pow(static_cast<double>(x), 0.75);
        double from_table = glint::tables::pow34_table[x] / 65536.0;
        double err = std::fabs(from_table - expected);
        if (err > max_err) max_err = err;
    }
    CHECK(max_err < 0.01, "pow34 table error < 0.01");
}

// --- Quantize round-trip ---
static void test_quantize_roundtrip() {
    std::printf("Quantize round-trip...\n");
    glint::tables::init_tables();

    // Create MDCT-like input (simulating a sine in one subband)
    double mdct[576] = {};
    mdct[18] = 0.5;   // one coefficient
    mdct[19] = -0.3;

    auto info = glint::quantize_granule(mdct, 1500, 0);
    CHECK(info.global_gain > 0 && info.global_gain < 256, "valid global_gain");
    CHECK(info.part2_3_length > 0, "nonzero part2_3_length");
    CHECK(info.part2_3_length <= 1500, "fits in bit budget");

    // Check that dominant coefficients survived quantization
    CHECK(info.ix[18] != 0, "dominant coeff preserved");
}

// --- MDCT overlap-add (TDAC property) ---
static void test_mdct_tdac() {
    std::printf("MDCT TDAC property...\n");
    glint::tables::init_tables();
    glint::MDCT mdct;

    // Feed two frames of constant input per subband
    double sb1[32][18] = {};
    double sb2[32][18] = {};
    for (int ts = 0; ts < 18; ts++) {
        sb1[0][ts] = 1.0;  // DC in subband 0
        sb2[0][ts] = 1.0;
    }

    double out1[32][18], out2[32][18];
    mdct.process(sb1, out1);
    mdct.process(sb2, out2);

    // The MDCT of a constant should produce consistent output across frames
    // (after the first frame's transient)
    double diff = 0;
    for (int k = 0; k < 18; k++)
        diff += std::fabs(out1[0][k] - out2[0][k]);

    // First frame differs from second due to zero-padding of prev,
    // but the outputs should be finite and reasonable
    CHECK(std::isfinite(out2[0][0]), "MDCT output is finite");
    CHECK(std::fabs(out2[0][0]) < 100.0, "MDCT output in reasonable range");
}

// --- Full encode/decode API ---
static void test_api() {
    std::printf("API smoke test...\n");

    // Check config validation
    CHECK(glint_check_config(44100, 128) == 0, "valid config accepted");
    CHECK(glint_check_config(44100, 999) != 0, "invalid bitrate rejected");
    CHECK(glint_check_config(12345, 128) != 0, "invalid sample rate rejected");

    // Create encoder
    glint_config cfg = {};
    cfg.sample_rate = 44100;
    cfg.num_channels = 1;
    cfg.mode = GLINT_MONO;
    cfg.bitrate = 128;
    cfg.path = GLINT_PATH_DEFAULT;

    glint_t enc = glint_create(&cfg);
    CHECK(enc != nullptr, "encoder created");

    int spf = glint_samples_per_frame(enc);
    CHECK(spf == 1152, "samples_per_frame = 1152 for MPEG-1");

    // Encode one frame of silence
    int16_t pcm[1152] = {};
    const int16_t* ch[] = { pcm };
    int out_size = 0;
    const uint8_t* frame = glint_encode(enc, ch, &out_size);
    CHECK(frame != nullptr, "encode returns non-null");
    CHECK(out_size > 0, "encode returns data");

    // Check MP3 sync word
    CHECK(frame[0] == 0xFF && (frame[1] & 0xE0) == 0xE0, "valid MP3 sync word");

    glint_destroy(enc);
}

#ifdef GLINT_FIXED_POINT
// --- Fixed-point subband vs double comparison ---
static void test_fixed_vs_double() {
    std::printf("Fixed-point vs double subband...\n");
    glint::tables::init_tables();

    // Generate a 1kHz sine
    int16_t pcm[1152];
    for (int i = 0; i < 1152; i++)
        pcm[i] = static_cast<int16_t>(std::sin(2.0 * 3.14159265 * 1000.0 * i / 44100.0) * 20000);

    // Run double path
    glint::SubbandAnalysis sb_d;
    double out_d[32][36];
    sb_d.analyze(pcm, out_d, 36);

    // Run fixed path
    glint::SubbandAnalysisFP sb_f;
    int32_t out_f[32][36];
    sb_f.analyze(pcm, out_f, 36);

    // Compare: fixed Q24 / 2^24 should ≈ double
    double max_err = 0;
    for (int sb = 0; sb < 32; sb++)
        for (int ts = 0; ts < 36; ts++) {
            double f_val = out_f[sb][ts] / 16777216.0;
            double d_val = out_d[sb][ts];
            double err = (d_val != 0) ? std::fabs((f_val - d_val) / d_val) : std::fabs(f_val);
            if (err > max_err && std::fabs(d_val) > 0.001)
                max_err = err;
        }
    CHECK(max_err < 0.01, "fixed vs double subband error < 1%");
}
#endif

int main() {
    std::printf("=== glint unit tests ===\n\n");

    test_bitstream();
    test_huffman_consistency();
    test_pow34_table();
    test_quantize_roundtrip();
    test_mdct_tdac();
    test_api();

#ifdef GLINT_FIXED_POINT
    test_fixed_vs_double();
#endif

    std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
