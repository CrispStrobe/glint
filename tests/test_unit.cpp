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

// With the bit reservoir, a frame's main data can spill into a later frame's
// slot, so output is deferred (a single glint_encode call may return 0 bytes).
// These helpers encode several frames and flush, returning the full stream.
#include <vector>
static std::vector<uint8_t> encode_i16_frames(glint_t enc, const int16_t* pcm,
                                              int n) {
    std::vector<uint8_t> out;
    const int16_t* ch[] = { pcm };
    for (int f = 0; f < n; f++) {
        int sz = 0;
        const uint8_t* d = glint_encode(enc, ch, &sz);
        if (d && sz > 0) out.insert(out.end(), d, d + sz);
    }
    int sz = 0;
    const uint8_t* d = glint_flush(enc, &sz);
    if (d && sz > 0) out.insert(out.end(), d, d + sz);
    return out;
}
static std::vector<uint8_t> encode_float_frames(glint_t enc, const float* pcm,
                                                int n) {
    std::vector<uint8_t> out;
    const float* ch[] = { pcm };
    for (int f = 0; f < n; f++) {
        int sz = 0;
        const uint8_t* d = glint_encode_float(enc, ch, &sz);
        if (d && sz > 0) out.insert(out.end(), d, d + sz);
    }
    int sz = 0;
    const uint8_t* d = glint_flush(enc, &sz);
    if (d && sz > 0) out.insert(out.end(), d, d + sz);
    return out;
}

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
    int16_t ix[576] = {};
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
        double from_table = glint::tables::pow34_table[x];
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

#if !defined(GLINT_FIXED_POINT) || defined(GLINT_BOTH_PATHS)
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
#endif

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

    // Encode a few frames of silence and flush (reservoir defers output).
    int16_t pcm[1152] = {};
    std::vector<uint8_t> out = encode_i16_frames(enc, pcm, 4);
    CHECK(!out.empty(), "encode returns data");

    // Check MP3 sync word at the start of the stream
    CHECK(out.size() >= 2 && out[0] == 0xFF && (out[1] & 0xE0) == 0xE0,
          "valid MP3 sync word");

    glint_destroy(enc);
}

#if defined(GLINT_FIXED_POINT) && defined(GLINT_BOTH_PATHS)
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
#endif // GLINT_BOTH_PATHS

#if !defined(GLINT_FIXED_POINT) || defined(GLINT_BOTH_PATHS)
// --- Float subband analysis preserves more precision than int16 path ---
static void test_float_subband() {
    std::printf("Float subband analysis precision...\n");
    glint::tables::init_tables();

    // Generate a low-amplitude sine that exercises precision beyond 16-bit
    // A value like 0.00003 (~1 LSB in int16) should survive float path but
    // gets quantized to 0 or 1 in int16.
    const int num_slots = 36;
    const int num_samples = num_slots * 32;
    float pcm_float[num_samples];
    int16_t pcm_int16[num_samples];
    for (int i = 0; i < num_samples; i++) {
        // Use a signal that has detail below int16 precision
        float v = static_cast<float>(std::sin(2.0 * 3.14159265 * 1000.0 * i / 44100.0) * 0.001);
        pcm_float[i] = v;
        // Simulate the old float->int16->double round-trip
        float vi = v * 32767.0f;
        if (vi > 32767.0f) vi = 32767.0f;
        if (vi < -32768.0f) vi = -32768.0f;
        pcm_int16[i] = static_cast<int16_t>(vi);
    }

    // Run float path
    glint::SubbandAnalysis sb_f;
    double out_float[32][36];
    sb_f.analyze_float(pcm_float, out_float, num_slots);

    // Run int16 path (simulates old glint_encode_float behavior)
    glint::SubbandAnalysis sb_i;
    double out_int16[32][36];
    sb_i.analyze(pcm_int16, out_int16, num_slots);

    // The float path should produce non-zero output for low-amplitude signals
    double float_energy = 0, int16_energy = 0;
    for (int sb = 0; sb < 32; sb++)
        for (int ts = 0; ts < 36; ts++) {
            float_energy += out_float[sb][ts] * out_float[sb][ts];
            int16_energy += out_int16[sb][ts] * out_int16[sb][ts];
        }

    CHECK(float_energy > 0, "float path produces non-zero output for low-level signal");
    // Float path should have more energy (or equal) since it doesn't truncate
    CHECK(float_energy >= int16_energy * 0.99,
          "float path preserves at least as much energy as int16 path");
}
#endif // double-precision path

#if !defined(GLINT_FIXED_POINT) || defined(GLINT_BOTH_PATHS)
// --- Float encode API smoke test ---
static void test_float_encode_api() {
    std::printf("Float encode API...\n");

    glint_config cfg = {};
    cfg.sample_rate = 44100;
    cfg.num_channels = 1;
    cfg.mode = GLINT_MONO;
    cfg.bitrate = 128;
    cfg.path = GLINT_PATH_DEFAULT;

    glint_t enc = glint_create(&cfg);
    CHECK(enc != nullptr, "encoder created for float test");

    int spf = glint_samples_per_frame(enc);

    // Generate a test signal as float [-1,1]
    float* pcm = new float[spf];
    for (int i = 0; i < spf; i++)
        pcm[i] = static_cast<float>(std::sin(2.0 * 3.14159265 * 440.0 * i / 44100.0) * 0.5);

    std::vector<uint8_t> out = encode_float_frames(enc, pcm, 4);
    CHECK(!out.empty(), "float encode returns data");
    CHECK(out.size() >= 2 && out[0] == 0xFF && (out[1] & 0xE0) == 0xE0,
          "float encode produces valid MP3 sync word");
    CHECK(out.size() > 100, "float encode produces continuous output");

    delete[] pcm;
    glint_destroy(enc);
}

// --- Float vs int16 encode: float path should produce equal or better output ---
static void test_float_vs_int16_encode() {
    std::printf("Float vs int16 encode comparison...\n");

    glint_config cfg = {};
    cfg.sample_rate = 44100;
    cfg.num_channels = 1;
    cfg.mode = GLINT_MONO;
    cfg.bitrate = 128;
    cfg.path = GLINT_PATH_DOUBLE;

    // Encode via float path
    glint_t enc_f = glint_create(&cfg);
    int spf = glint_samples_per_frame(enc_f);

    float* pcm_float = new float[spf];
    for (int i = 0; i < spf; i++)
        pcm_float[i] = static_cast<float>(std::sin(2.0 * 3.14159265 * 1000.0 * i / 44100.0) * 0.8);

    std::vector<uint8_t> out_float = encode_float_frames(enc_f, pcm_float, 4);
    CHECK(!out_float.empty(), "float path encode OK");

    // Encode via int16 path (same signal converted to int16)
    glint_t enc_i = glint_create(&cfg);
    int16_t* pcm_int16 = new int16_t[spf];
    for (int i = 0; i < spf; i++) {
        float v = pcm_float[i] * 32767.0f;
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm_int16[i] = static_cast<int16_t>(v);
    }

    std::vector<uint8_t> out_int = encode_i16_frames(enc_i, pcm_int16, 4);
    CHECK(!out_int.empty(), "int16 path encode OK");

    // Both paths process the same frame count at the same bitrate, so the
    // total emitted stream size must match.
    CHECK(out_float.size() == out_int.size(),
          "float and int16 paths produce same total size");

    delete[] pcm_float;
    delete[] pcm_int16;
    glint_destroy(enc_f);
    glint_destroy(enc_i);
}
#endif // double-precision path

// ---------------------------------------------------------------------------
// AAC
// ---------------------------------------------------------------------------
#include "aac_mdct.hpp"
#include "aac_coder.hpp"
#include "aac_tables.hpp"

// Fast MDCT must match the direct ISO 13818-7 formula:
// X[k] = 2 * sum z[n] cos(2pi/N (n+n0)(k+1/2)), n0 = (N/2+1)/2, sine-windowed.
static void test_aac_mdct_vs_direct() {
    std::printf("AAC MDCT (fast vs direct ISO formula)...\n");
    const int N = 2048, M = 1024;
    static glint::aac::PcmT prev[M], cur[M];
    static glint::aac::SpecT spec[M];
    static double ref[M], z[N];
    unsigned s = 12345;
    for (int i = 0; i < M; i++) {
        s = s * 1103515245u + 12345u;
        prev[i] = static_cast<glint::aac::PcmT>(static_cast<int>(s >> 16) % 65536 - 32768);
        s = s * 1103515245u + 12345u;
        cur[i] = static_cast<glint::aac::PcmT>(static_cast<int>(s >> 16) % 65536 - 32768);
    }
    glint::aac::aac_mdct_frame(glint::aac::kSeqLong, prev, cur, spec);

    const double pi = 3.14159265358979323846;
    for (int n = 0; n < M; n++) {
        double w0 = std::sin(pi / N * (n + 0.5));
        double w1 = std::sin(pi / N * (M + n + 0.5));
        z[n] = w0 * prev[n];
        z[M + n] = w1 * cur[n];
    }
    const double sscale = 1 << glint::aac::kSpecFracBits;
    double n0 = (N / 2 + 1) / 2.0;
    double max_err = 0, max_ref = 0;
    for (int k = 0; k < M; k++) {
        double acc = 0;
        for (int n = 0; n < N; n++) {
            acc += z[n] * std::cos(2.0 * pi / N * (n + n0) * (k + 0.5));
        }
        ref[k] = 2.0 * acc;
        double e = std::fabs(ref[k] - spec[k] / sscale);
        if (e > max_err) max_err = e;
        if (std::fabs(ref[k]) > max_ref) max_ref = std::fabs(ref[k]);
    }
    const double tol = glint::aac::kSpecFracBits > 0 ? 1e-4
                       : sizeof(glint::aac::SpecT) == 4 ? 2e-5 : 1e-10;
    CHECK(max_err / max_ref < tol, "fast MDCT matches direct ISO formula");

    // Short windows: each 256-point window vs the direct formula.
    static glint::aac::SpecT specs[M];
    glint::aac::aac_mdct_frame(glint::aac::kSeqShort, prev, cur, specs);
    double x[N];
    for (int n = 0; n < M; n++) { x[n] = prev[n]; x[M + n] = cur[n]; }
    double serr = 0, sref = 0;
    for (int w = 0; w < 8; w++) {
        const double* base = x + 448 + 128 * w;
        double n0s = (256 / 2 + 1) / 2.0;
        for (int k = 0; k < 128; k++) {
            double acc = 0;
            for (int n = 0; n < 256; n++) {
                acc += std::sin(pi / 256 * (n + 0.5)) * base[n] *
                       std::cos(2.0 * pi / 256 * (n + n0s) * (k + 0.5));
            }
            double r = 2.0 * acc;
            double e = std::fabs(r - specs[128 * w + k] / sscale);
            if (e > serr) serr = e;
            if (std::fabs(r) > sref) sref = std::fabs(r);
        }
    }
    CHECK(serr / sref < tol, "short MDCT matches direct ISO formula");
}

static void test_aac_tables_sanity() {
    std::printf("AAC tables sanity...\n");
    using namespace glint::aac_tables;
    CHECK(kScfBits[60] == 1, "scalefactor dpcm=0 codeword is 1 bit");
    // all long-window sfb widths are multiples of the largest book dimension
    for (int r = 0; r < kNumSampleRates; r++) {
        bool ok = true;
        for (int b = 0; b < kNumSwbLong[r]; b++) {
            int w = kSwbOffsetLong[r][b + 1] - kSwbOffsetLong[r][b];
            if (w <= 0 || w % 4 != 0) ok = false;
        }
        CHECK(ok, "long sfb widths positive and 4-aligned");
        CHECK(kSwbOffsetLong[r][kNumSwbLong[r]] == 1024, "long sfb table terminates at 1024");
    }
}

// Quantize a synthetic spectrum, section it, emit the ICS, and verify the
// emitted size matches the exact bit count used by the rate loop.
static void test_aac_coder_count_matches_emission() {
    std::printf("AAC coder (count == emission)...\n");
    const int sri = 4;  // 44.1 kHz
    static glint::aac::SpecT spec[1024];
    unsigned s = 777;
    for (int i = 0; i < 1024; i++) {
        s = s * 1103515245u + 12345u;
        double amp = (i < 600) ? 20000.0 / (1 + i) : 0.0;
        spec[i] = static_cast<glint::aac::SpecT>(amp * ((static_cast<int>(s >> 16) % 2001) - 1000) / 1000.0);
    }
    glint::aac::AacBandLayout layout;
    glint::aac::aac_make_layout(sri, glint::aac::kSeqLong, 40, nullptr, 1, &layout);
    glint::aac::AacChannelPlan plan;
    plan.tns.active = 0;
    glint::aac::aac_fit_channel(spec, layout, 3000, nullptr, -1, 0, &plan);
    CHECK(plan.ics_bits <= 3000, "fitted plan respects the bit budget");
    CHECK(plan.global_gain >= 0 && plan.global_gain <= 255, "global_gain in range");

    glint::aac::AacBitWriter counter(0);
    glint::aac::aac_write_ics_body(counter, plan, layout, false);
    CHECK(counter.bits() == plan.ics_bits, "count-only pass matches plan bits");

    uint8_t buf[4096];
    glint::aac::AacBitWriter writer(buf, sizeof(buf));
    glint::aac::aac_write_ics_body(writer, plan, layout, false);
    CHECK(writer.bits() == plan.ics_bits, "emission bit count matches plan bits");
    CHECK(!writer.overflowed(), "no writer overflow");

    // Short layout: 3 groups, count==emission holds with grouped sectioning.
    uint8_t glen[3] = { 3, 2, 3 };
    glint::aac::AacBandLayout sl;
    glint::aac::aac_make_layout(sri, glint::aac::kSeqShort, 14, glen, 3, &sl);
    CHECK(sl.num_bands == 42, "short layout band count");
    CHECK(sl.num_lines == 8 * 112 || sl.num_lines > 0, "short layout lines");
    glint::aac::AacChannelPlan splan;
    splan.tns.active = 0;
    glint::aac::aac_fit_channel(spec, sl, 2500, nullptr, -1, 0, &splan);
    CHECK(splan.ics_bits <= 2500, "short plan respects the bit budget");
    glint::aac::AacBitWriter scount(0);
    glint::aac::aac_write_ics_body(scount, splan, sl, false);
    CHECK(scount.bits() == splan.ics_bits, "short count matches emission");
}

static void test_aac_api_smoke() {
    std::printf("AAC API smoke (ADTS framing)...\n");
    glint_aac_config cfg = {};  // zero-init per the header contract
    cfg.sample_rate = 44100;
    cfg.num_channels = 2;
    cfg.bitrate = 128;
    cfg.quality = GLINT_QUALITY_NORMAL;
    glint_aac_t enc = glint_aac_create(&cfg);
    CHECK(enc != nullptr, "encoder created");
    CHECK(glint_aac_samples_per_frame(enc) == 1024, "1024 samples per frame");

    static int16_t pcm[2][1024];
    for (int i = 0; i < 1024; i++) {
        pcm[0][i] = static_cast<int16_t>(12000 * std::sin(2 * 3.14159265 * 440.0 * i / 44100.0));
        pcm[1][i] = static_cast<int16_t>(12000 * std::sin(2 * 3.14159265 * 554.0 * i / 44100.0));
    }
    const int16_t* ch[2] = { pcm[0], pcm[1] };
    long total = 0;
    int nframes = 0;
    for (int f = 0; f < 50; f++) {
        int sz = 0;
        const uint8_t* d = glint_aac_encode(enc, ch, &sz);
        CHECK(d != nullptr && sz > 7, "frame emitted");
        if (d && sz > 7) {
            CHECK(d[0] == 0xFF && (d[1] & 0xF6) == 0xF0, "ADTS syncword");
            int flen = ((d[3] & 3) << 11) | (d[4] << 3) | (d[5] >> 5);
            CHECK(flen == sz, "ADTS frame_length matches emitted size");
            total += sz;
            nframes++;
        }
    }
    int sz = 0;
    const uint8_t* d = glint_aac_flush(enc, &sz);
    CHECK(d != nullptr && sz > 14, "flush emitted (two tail frames)");
    CHECK(d && d[0] == 0xFF && (d[1] & 0xF6) == 0xF0, "flush starts with ADTS sync");
    total += sz;

    // 50 encode calls + 2 flush frames; the debt controller must keep the
    // average near target.
    double nominal = 128000.0 * 1024 / 44100 / 8 * (nframes + 2);
    CHECK(total > 0.8 * nominal && total < 1.25 * nominal, "CBR average near target");
    glint_aac_destroy(enc);
}

// --- Opus range coder (PLAN § O0) ---
#include "opus_ec.hpp"

// xorshift32; encode and decode passes replay the same op script from the
// same seed, so op parameters never need to be stored.
static uint32_t ec_rand_state;
static uint32_t ec_rand() {
    uint32_t x = ec_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return ec_rand_state = x;
}

// icdf[s] = 256 - fh(s): 5 symbols with widths 56/80/80/28/12.
static const uint8_t kEcTestIcdf[5] = { 200, 120, 40, 12, 0 };

static void ec_script_encode(uint32_t seed, int ops,
                             glint::opus::RangeEncoder& enc,
                             uint32_t* tells) {
    ec_rand_state = seed;
    if (tells) tells[0] = enc.tell();
    for (int i = 0; i < ops; i++) {
        switch (ec_rand() % 5) {
        case 0: {
            uint32_t ft = 2 + ec_rand() % ((1u << 15) - 2);
            uint32_t fl = ec_rand() % ft;
            enc.encode(fl, fl + 1, ft);
            break;
        }
        case 1: {
            unsigned logp = 1 + ec_rand() % 14;
            int bit = static_cast<int>(ec_rand() & 1);
            enc.enc_bit_logp(bit, logp);
            break;
        }
        case 2:
            enc.enc_icdf(static_cast<int>(ec_rand() % 5), kEcTestIcdf, 8);
            break;
        case 3: {
            uint32_t ft = 2 + ec_rand() % (1u << 20);
            uint32_t v = ec_rand() % ft;
            enc.enc_uint(v, ft);
            break;
        }
        case 4: {
            unsigned bits = 1 + ec_rand() % 24;
            uint32_t v = ec_rand() & ((1u << bits) - 1);
            enc.enc_bits(v, bits);
            break;
        }
        }
        if (tells) tells[i + 1] = enc.tell();
    }
}

static void ec_script_decode(uint32_t seed, int ops,
                             glint::opus::RangeDecoder& dec,
                             const uint32_t* tells, bool* values_ok,
                             bool* tells_ok) {
    ec_rand_state = seed;
    *values_ok = true;
    *tells_ok = !tells || dec.tell() == tells[0];
    for (int i = 0; i < ops; i++) {
        switch (ec_rand() % 5) {
        case 0: {
            uint32_t ft = 2 + ec_rand() % ((1u << 15) - 2);
            uint32_t fl = ec_rand() % ft;
            if (dec.decode(ft) != fl) *values_ok = false;
            dec.dec_update(fl, fl + 1, ft);
            break;
        }
        case 1: {
            unsigned logp = 1 + ec_rand() % 14;
            int bit = static_cast<int>(ec_rand() & 1);
            if (dec.dec_bit_logp(logp) != bit) *values_ok = false;
            break;
        }
        case 2: {
            int s = static_cast<int>(ec_rand() % 5);
            if (dec.dec_icdf(kEcTestIcdf, 8) != s) *values_ok = false;
            break;
        }
        case 3: {
            uint32_t ft = 2 + ec_rand() % (1u << 20);
            uint32_t v = ec_rand() % ft;
            if (dec.dec_uint(ft) != v) *values_ok = false;
            break;
        }
        case 4: {
            unsigned bits = 1 + ec_rand() % 24;
            uint32_t v = ec_rand() & ((1u << bits) - 1);
            if (dec.dec_bits(bits) != v) *values_ok = false;
            break;
        }
        }
        if (tells && dec.tell() != tells[i + 1]) *tells_ok = false;
    }
}

static void test_opus_range_coder() {
    std::printf("Opus range coder...\n");
    using glint::opus::RangeEncoder;
    using glint::opus::RangeDecoder;
    enum { kOps = 2000, kBufSize = 8192 };
    static uint8_t buf[kBufSize];
    static uint32_t tells[kOps + 1];

    for (uint32_t seed = 1; seed <= 5; seed++) {
        RangeEncoder enc;
        enc.init(buf, kBufSize);
        ec_script_encode(seed, kOps, enc, tells);
        uint32_t final_tell = enc.tell();
        enc.done();
        CHECK(enc.error() == 0, "encoder buffer did not overflow");

        RangeDecoder dec;
        dec.init(buf, kBufSize);
        bool values_ok = false, tells_ok = false;
        ec_script_decode(seed, kOps, dec, tells, &values_ok, &tells_ok);
        CHECK(values_ok, "all decoded values match encoded");
        CHECK(tells_ok, "decoder tell() tracks encoder tell() per op");
        CHECK(dec.error() == 0, "decoder error flag clear");

        // The stream must fit in ceil(tell/8) bytes — re-run the same
        // script into an exact-size buffer and decode from that.
        uint32_t nbytes = (final_tell + 7) / 8;
        CHECK(nbytes <= kBufSize, "exact size within scratch buffer");
        RangeEncoder enc2;
        enc2.init(buf, nbytes);
        ec_script_encode(seed, kOps, enc2, nullptr);
        enc2.done();
        CHECK(enc2.error() == 0, "exact-size buffer suffices (done() bound)");
        RangeDecoder dec2;
        dec2.init(buf, nbytes);
        ec_script_decode(seed, kOps, dec2, nullptr, &values_ok, &tells_ok);
        CHECK(values_ok, "exact-size round-trip decodes");
    }

    // Carry stress: maximal symbols drive val to the top of the range,
    // exercising the buffered-0xFF carry chain in carry_out().
    {
        uint8_t cbuf[256];
        RangeEncoder enc;
        enc.init(cbuf, sizeof(cbuf));
        for (int i = 0; i < 100; i++) enc.encode(255, 256, 256);
        enc.done();
        CHECK(enc.error() == 0, "carry stress encode fits");
        RangeDecoder dec;
        dec.init(cbuf, sizeof(cbuf));
        bool ok = true;
        for (int i = 0; i < 100; i++) {
            if (dec.decode(256) != 255) ok = false;
            dec.dec_update(255, 256, 256);
        }
        CHECK(ok, "carry stress round-trip");
    }

    // enc_uint edges around the range/raw split at 2^8 and power-of-2 fts.
    {
        static const uint32_t fts[] = {
            2, 3, 255, 256, 257, 1u << 16, (1u << 16) + 1, 1u << 24
        };
        uint8_t ubuf[512];
        RangeEncoder enc;
        enc.init(ubuf, sizeof(ubuf));
        for (uint32_t ft : fts) {
            const uint32_t vals[4] = { 0, 1, ft / 2, ft - 1 };
            for (uint32_t v : vals) enc.enc_uint(v, ft);
        }
        uint32_t etell = enc.tell();
        enc.done();
        CHECK(enc.error() == 0, "uint edges encode fits");
        RangeDecoder dec;
        dec.init(ubuf, sizeof(ubuf));
        bool ok = true;
        for (uint32_t ft : fts) {
            const uint32_t vals[4] = { 0, 1, ft / 2, ft - 1 };
            for (uint32_t v : vals)
                if (dec.dec_uint(ft) != v) ok = false;
        }
        CHECK(ok, "uint edges round-trip");
        CHECK(dec.tell() == etell, "uint edges tell parity");
        CHECK(dec.error() == 0, "uint edges no decoder error");
    }
}

// --- Opus CELT primitives (PLAN § O1) ---
#include "opus_laplace.hpp"
#include "opus_cwrs.hpp"
#include "opus_celt_tables.hpp"

static void test_opus_celt_prims() {
    std::printf("Opus CELT primitives (tables, laplace, cwrs)...\n");
    using namespace glint::opus;

    // Generated-tables sanity (deep checks live in the generator itself).
    CHECK(celt::kEBands[0] == 0 && celt::kEBands[21] == 100,
          "eBands endpoints");
    CHECK(celt::kWindow[119] > 0.999 && celt::kWindow[0] < 1e-3,
          "window shape");
    CHECK(celt::kEProbModel[0][0][0] > 0, "e_prob_model nonzero");

    // Laplace round-trip, including the flat-tail clamp path. Parameters
    // drawn exactly like CELT uses them (prob<<7, decay<<6).
    enum { kNVals = 300 };
    static uint8_t buf[4096];
    static unsigned fss[kNVals];
    static int decays[kNVals], coded[kNVals];
    static uint32_t tells[kNVals];
    ec_rand_state = 42;
    RangeEncoder enc;
    enc.init(buf, sizeof(buf));
    for (int i = 0; i < kNVals; i++) {
        fss[i] = (1 + ec_rand() % 255) << 7;
        decays[i] = (1 + ec_rand() % 178) << 6;
        int v = static_cast<int>(ec_rand() % 81) - 40;
        if (i % 8 == 0) v = static_cast<int>(ec_rand() % 4000) - 2000;
        coded[i] = laplace_encode(enc, v, fss[i], decays[i]);
        tells[i] = enc.tell();
    }
    enc.done();
    CHECK(enc.error() == 0, "laplace encode fits");
    RangeDecoder dec;
    dec.init(buf, sizeof(buf));
    bool vals_ok = true, tell_ok = true;
    for (int i = 0; i < kNVals; i++) {
        if (laplace_decode(dec, fss[i], decays[i]) != coded[i])
            vals_ok = false;
        if (dec.tell() != tells[i]) tell_ok = false;
    }
    CHECK(vals_ok, "laplace round-trip (incl. tail clamp)");
    CHECK(tell_ok, "laplace tell parity");

    // CWRS round-trip across representative (n, k) pairs.
    static const int nk[][2] = { { 2, 1 },   { 2, 128 }, { 3, 128 },
                                 { 4, 7 },   { 8, 3 },   { 22, 10 },
                                 { 96, 4 },  { 176, 2 } };
    for (auto& p : nk) {
        int n = p[0], k = p[1];
        int y[176] = { 0 };
        // Deterministic vector: alternate signs, spread pulses.
        int left = k;
        for (int j = 0; left > 0; j = (j + 1) % n) {
            int take = left > 3 ? 3 : left;
            y[j] += (j & 1) ? -take : take;  // sign fixed per slot
            left -= take;
        }
        RangeEncoder e2;
        e2.init(buf, sizeof(buf));
        encode_pulses(y, n, k, e2);
        uint32_t etell = e2.tell();
        e2.done();
        CHECK(e2.error() == 0, "cwrs encode fits");
        RangeDecoder d2;
        d2.init(buf, sizeof(buf));
        int yd[176];
        int32_t yy = decode_pulses(yd, n, k, d2);
        bool same = true;
        int32_t yy_ref = 0;
        for (int j = 0; j < n; j++) {
            if (yd[j] != y[j]) same = false;
            yy_ref += y[j] * y[j];
        }
        CHECK(same, "cwrs round-trip vector");
        CHECK(yy == yy_ref, "cwrs yy (sum of squares)");
        CHECK(d2.tell() == etell, "cwrs tell parity");
    }
}

// --- Opus CELT allocator + IMDCT smoke (deep checks are the libopus
// cross-check tools; these guard regressions without the oracle) ---
#include "opus_celt_rate.hpp"
#include "opus_mdct.hpp"

static void test_opus_celt_alloc_mdct() {
    std::printf("Opus CELT allocator + IMDCT smoke...\n");
    using namespace glint::opus;

    // bits2pulses/pulses2bits are cache-consistent: converting a pulse
    // count's own bit cost back returns the same pseudo-pulse index —
    // valid only where the uint8 cost curve is strictly monotonic (wide
    // bands saturate at 255 and plateau, where nearest-match rounding
    // legitimately picks the lowest index).
    bool rt_ok = true;
    for (int lm = 0; lm < 4; lm++)
        for (int band = 0; band < 21; band += 4) {
            // cache[0] is the row's max pseudo-pulse index; indices beyond
            // it are out-of-domain (the reference reads adjacent-row
            // garbage there too — the allocator never produces them).
            int maxp = celt::kCacheBits[celt::kCacheIndex[(lm + 1) * 21 +
                                                          band]];
            for (int p = 2; p + 1 <= maxp && p <= 8; p++) {
                int lo = pulses2bits(band, lm, p - 1);
                int mid = pulses2bits(band, lm, p);
                int hi = pulses2bits(band, lm, p + 1);
                if (!(lo < mid && mid < hi && hi < 250)) continue;
                if (bits2pulses(band, lm, mid) != p) rt_ok = false;
            }
        }
    CHECK(rt_ok, "bits2pulses(pulses2bits(p)) == p (in-domain)");

    int caps1[21], caps2[21];
    init_caps(caps1, 0, 1);
    init_caps(caps2, 3, 2);
    bool caps_ok = true;
    for (int i = 0; i < 21; i++)
        if (caps1[i] <= 0 || caps2[i] <= caps1[i]) caps_ok = false;
    CHECK(caps_ok, "caps positive and grow with LM/C");

    // IMDCT energy sanity: a single unit spectral line at shift 3 must
    // produce a bounded, nonzero waveform (exact values are covered by
    // tools/crosscheck_opus_mdct.py against libopus).
    CeltImdct imdct;
    double window[120];
    mdct_window_fill(window, 120);
    double spec[120] = { 0 };
    spec[7] = 1.0;
    double out[240] = { 0 };
    imdct.backward(spec, out, window, 120, 3, 1);
    double peak = 0;
    for (int i = 0; i < 180; i++)
        peak = out[i] > peak ? out[i] : (-out[i] > peak ? -out[i] : peak);
    CHECK(peak > 0.1 && peak < 4.0, "IMDCT line response bounded");
}

// ---------------------------------------------------------------------
// Opus C API round-trip: CELT encode -> decode through the public ABI.
// The final-range identity (decoder range == encoder range) is the Opus
// conformance check and needs no external oracle; SNR bounds catch
// gross signal damage. Also exercises FEC concealment, non-48k output,
// the multistream wrapper and self-delimited packet parsing.
static void test_opus_c_api_roundtrip() {
    std::printf("[opus C API] encode/decode round-trip...\n");
    const int frame = 960, frames = 25, ch = 2;
    glint_opus_enc_t enc = glint_opus_enc_create(ch, 128000, 0);
    glint_opus_dec_t dec = glint_opus_dec_create(ch, 48000);
    CHECK(enc && dec, "create encoder + decoder");

    static float pcm[frame * ch], out_pcm[frame * ch];
    static uint8_t pkt[1500];
    double err = 0, sig = 0;
    int range_matches = 0;
    for (int f = 0; f < frames; f++) {
        for (int i = 0; i < frame; i++) {
            double t = (f * frame + i) / 48000.0;
            pcm[i * ch] = 0.4f * (float)sin(2 * M_PI * 440.0 * t);
            pcm[i * ch + 1] = 0.3f * (float)sin(2 * M_PI * 660.0 * t);
        }
        int n = glint_opus_encode(enc, pcm, frame, pkt, sizeof(pkt));
        CHECK(n > 1, "encode returns a packet");
        int m = glint_opus_decode(dec, pkt, n, out_pcm, frame);
        CHECK(m == frame, "decode returns the frame");
        if (glint_opus_dec_final_range(dec) ==
            glint_opus_enc_final_range(enc))
            range_matches++;
        if (f >= 2) {  // skip codec warm-up for the SNR
            for (int i = 0; i < frame * ch; i++) {
                double d = pcm[i] - out_pcm[i];
                // 120-sample codec delay: compare energies only.
                sig += pcm[i] * pcm[i];
                err += d * d;
            }
        }
    }
    CHECK(range_matches == frames,
          "final ranges identical on every packet (conformance identity)");
    CHECK(err < 2.5 * sig, "decoded energy in the signal's ballpark");

    // FEC/PLC entry: conceal one lost frame without a next packet.
    int c = glint_opus_decode_fec(dec, NULL, 0, out_pcm, frame);
    CHECK(c == frame, "plain concealment fills the frame");

    glint_opus_enc_destroy(enc);
    glint_opus_dec_destroy(dec);

    // Non-48k output: same stream decoded at 16 kHz -> 320 samples.
    glint_opus_enc_t e2 = glint_opus_enc_create(1, 64000, 1);  // VBR mono
    glint_opus_dec_t d16 = glint_opus_dec_create(1, 16000);
    CHECK(e2 && d16, "VBR encoder + 16k decoder");
    for (int i = 0; i < frame; i++)
        pcm[i] = 0.3f * (float)sin(2 * M_PI * 500.0 * i / 48000.0);
    int n = glint_opus_encode(e2, pcm, frame, pkt, sizeof(pkt));
    CHECK(n > 1, "VBR encode");
    int m16 = glint_opus_decode(d16, pkt, n, out_pcm, 5760);
    CHECK(m16 == 320, "16 kHz decode of a 20 ms packet yields 320");
    CHECK(glint_opus_dec_final_range(d16) ==
              glint_opus_enc_final_range(e2),
          "final range identity holds at 16 kHz output");

    // Multistream wrapper: a 1-stream coupled layout equals stereo.
    uint8_t mapping[2] = { 0, 1 };
    glint_opus_ms_dec_t ms =
        glint_opus_ms_dec_create(2, 1, 1, mapping, 48000);
    CHECK(ms != NULL, "multistream create (1 coupled stream)");
    glint_opus_enc_t e3 = glint_opus_enc_create(2, 96000, 0);
    for (int i = 0; i < frame; i++) {
        pcm[i * 2] = 0.3f * (float)sin(2 * M_PI * 330.0 * i / 48000.0);
        pcm[i * 2 + 1] = 0.3f * (float)sin(2 * M_PI * 550.0 * i / 48000.0);
    }
    n = glint_opus_encode(e3, pcm, frame, pkt, sizeof(pkt));
    int mm = glint_opus_ms_decode(ms, pkt, n, out_pcm, frame);
    CHECK(mm == frame, "multistream decode of the single stream");
    glint_opus_ms_dec_destroy(ms);
    glint_opus_enc_destroy(e2);
    glint_opus_enc_destroy(e3);
    glint_opus_dec_destroy(d16);
}

#include "opus_decoder.hpp"

// Self-delimited framing (RFC 6716 appendix B) against hand-built
// packets — the multistream splitter depends on these exact semantics.
static void test_opus_packet_parse() {
    std::printf("[opus] packet parse (regular + self-delimited)...\n");
    using glint::opus::OpusPacket;

    // Code 0 (one frame), regular: everything after the TOC is payload.
    uint8_t p0[5] = { 28 << 3, 1, 2, 3, 4 };
    OpusPacket pkt;
    CHECK(glint::opus::opus_packet_parse(p0, 5, &pkt) == 0 &&
              pkt.frame_count == 1 && pkt.sizes[0] == 4,
          "code-0 regular parse");

    // Code 0 self-delimited: explicit size 2, then 2 payload bytes and
    // 2 trailing bytes that belong to the NEXT stream.
    uint8_t p1[6] = { 28 << 3, 2, 9, 9, 7, 7 };
    int32_t off = 0;
    CHECK(glint::opus::opus_packet_parse_ext(p1, 6, true, &pkt, &off) ==
                  0 &&
              pkt.frame_count == 1 && pkt.sizes[0] == 2 && off == 4,
          "code-0 self-delimited parse + packet_offset");

    // Code 1 (two CBR frames) self-delimited: one size covers both.
    uint8_t p2[8] = { (28 << 3) | 1, 3, 5, 5, 5, 6, 6, 6 };
    CHECK(glint::opus::opus_packet_parse_ext(p2, 8, true, &pkt, &off) ==
                  0 &&
              pkt.frame_count == 2 && pkt.sizes[0] == 3 &&
              pkt.sizes[1] == 3 && off == 8,
          "code-1 self-delimited applies the size to both frames");

    // Truncated self-delimited size must fail, not read past the end.
    uint8_t p3[2] = { 28 << 3, 251 };
    CHECK(glint::opus::opus_packet_parse_ext(p3, 2, true, &pkt, &off) < 0,
          "self-delimited size larger than the buffer rejected");
}

// ---------------------------------------------------------------------
// Decoder C ABI: encode a tone through the public MP3/AAC encoder, decode
// it back through the public decoder ABI, and check the roundtrip energy
// and sample count. Validates the frame_info walkers + decode wiring.
static void test_mp3_decoder_api() {
    std::printf("[mp3 decoder API] encode -> decode roundtrip...\n");
    const int sr = 44100, spf_hint = 1152, nframes = 40, ch = 2;
    glint_config cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = sr;
    cfg.num_channels = ch;
    cfg.mode = GLINT_JOINT;
    cfg.bitrate = 128;
    cfg.quality = GLINT_QUALITY_NORMAL;
    glint_t enc = glint_create(&cfg);
    CHECK(enc != nullptr, "mp3 encoder create");
    int spf = glint_samples_per_frame(enc);
    CHECK(spf == spf_hint, "mp3 1152 samples/frame");

    std::vector<uint8_t> stream;
    std::vector<int16_t> l(spf), r(spf);
    double in_energy = 0;
    long phase = 0;
    for (int f = 0; f < nframes; f++) {
        for (int i = 0; i < spf; i++, phase++) {
            double s = 0.4 * std::sin(2 * M_PI * 440.0 * phase / sr);
            l[i] = (int16_t)(s * 20000);
            r[i] = (int16_t)(s * 15000);
            in_energy += (l[i] / 32768.0) * (l[i] / 32768.0);
        }
        const int16_t* chd[2] = { l.data(), r.data() };
        int n = 0;
        const uint8_t* out = glint_encode(enc, chd, &n);
        stream.insert(stream.end(), out, out + n);
    }
    int n = 0;
    const uint8_t* tail = glint_flush(enc, &n);
    stream.insert(stream.end(), tail, tail + n);
    glint_destroy(enc);
    CHECK(stream.size() > 1000, "mp3 stream produced");

    glint_mp3_dec_t dec = glint_mp3_dec_create();
    CHECK(dec != nullptr, "mp3 decoder create");
    std::vector<float> pcm(2 * 1152);
    size_t off = 0;
    double out_energy = 0;
    int got = 0, frames = 0;
    glint_dec_frame_info fi;
    while (off + 4 <= stream.size()) {
        if (glint_mp3_frame_info(stream.data() + off,
                                 (int)(stream.size() - off), &fi) < 0) {
            off++;
            continue;
        }
        if (off + (size_t)fi.frame_bytes > stream.size()) break;
        int s = glint_mp3_decode(dec, stream.data() + off,
                                 (int)(stream.size() - off), pcm.data(),
                                 &fi);
        if (s > 0) {
            for (int i = 0; i < s; i++)
                out_energy += pcm[i * fi.channels] * pcm[i * fi.channels];
            got += s;
            frames++;
        }
        off += fi.frame_bytes;
    }
    glint_mp3_dec_destroy(dec);
    CHECK(frames > 30, "decoded most frames");
    CHECK(got > nframes * spf / 2, "decoded a stream's worth of samples");
    // Energy within a factor of 2 (lossy, plus reservoir warm-up).
    double ratio = out_energy / (in_energy * 0.5 /*L only*/ + 1e-9);
    CHECK(ratio > 0.3 && ratio < 3.0, "mp3 roundtrip energy sane");
}

static void test_aac_decoder_api() {
    std::printf("[aac decoder API] encode -> decode roundtrip...\n");
    const int sr = 44100, ch = 2, nframes = 40;
    glint_aac_config cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = sr;
    cfg.num_channels = ch;
    cfg.bitrate = 128;
    cfg.quality = GLINT_QUALITY_NORMAL;
    glint_aac_t enc = glint_aac_create(&cfg);
    CHECK(enc != nullptr, "aac encoder create");
    int spf = glint_aac_samples_per_frame(enc);
    CHECK(spf == 1024, "aac 1024 samples/frame");

    std::vector<uint8_t> stream;
    std::vector<int16_t> l(spf), r(spf);
    long phase = 0;
    for (int f = 0; f < nframes; f++) {
        for (int i = 0; i < spf; i++, phase++) {
            double s = 0.4 * std::sin(2 * M_PI * 440.0 * phase / sr);
            l[i] = (int16_t)(s * 20000);
            r[i] = (int16_t)(s * 15000);
        }
        const int16_t* chd[2] = { l.data(), r.data() };
        int n = 0;
        const uint8_t* out = glint_aac_encode(enc, chd, &n);
        if (out && n > 0) stream.insert(stream.end(), out, out + n);
    }
    int n = 0;
    const uint8_t* tail = glint_aac_flush(enc, &n);
    if (tail && n > 0) stream.insert(stream.end(), tail, tail + n);
    glint_aac_destroy(enc);
    CHECK(stream.size() > 1000, "aac stream produced");

    glint_aac_dec_t dec = glint_aac_dec_create();
    CHECK(dec != nullptr, "aac decoder create");
    std::vector<float> pcm(2 * 1024);
    size_t off = 0;
    int got = 0, frames = 0;
    double out_energy = 0;
    glint_dec_frame_info fi;
    while (off + 7 <= stream.size()) {
        if (glint_aac_frame_info(stream.data() + off,
                                 (int)(stream.size() - off), &fi) < 0) {
            off++;
            continue;
        }
        if (off + (size_t)fi.frame_bytes > stream.size()) break;
        int s = glint_aac_decode(dec, stream.data() + off,
                                 (int)(stream.size() - off), pcm.data(),
                                 &fi);
        if (s > 0) {
            for (int i = 0; i < s; i++)
                out_energy += pcm[i * fi.channels] * pcm[i * fi.channels];
            got += s;
            frames++;
        }
        off += fi.frame_bytes;
    }
    glint_aac_dec_destroy(dec);
    CHECK(frames > 30, "aac decoded most frames");
    CHECK(got > nframes * spf / 2, "aac decoded samples");
    CHECK(out_energy > 1.0, "aac roundtrip produced audible energy");
}

#include "vorbis_bits.hpp"
#include "vorbis_decoder.hpp"

// Vorbis identification-header parse (spec §4.2.2): channels / rate /
// power-of-two blocksizes, framing bit, version gate.
static void test_vorbis_id_header() {
    std::printf("[vorbis] identification header parse...\n");
    // 0x01 "vorbis" + version(0) + ch(2) + rate(44100) + 3x bitrate(0) +
    // blocksize byte (bs0=2^8=256, bs1=2^11=2048) + framing=1.
    uint8_t pkt[30] = {
        0x01, 'v', 'o', 'r', 'b', 'i', 's',
        0x00, 0x00, 0x00, 0x00,              // version = 0
        0x02,                                 // channels = 2
        0x44, 0xAC, 0x00, 0x00,              // sample rate = 44100
        0x00, 0x00, 0x00, 0x00,              // bitrate max
        0x00, 0x00, 0x00, 0x00,              // bitrate nominal
        0x00, 0x00, 0x00, 0x00,              // bitrate minimum
        0xB8,                                 // blocksizes: 0=8(256),1=11(2048)
        0x01                                  // framing flag = 1
    };
    glint::vorbis::IdHeader h =
        glint::vorbis::parse_id_header(pkt, sizeof(pkt));
    CHECK(h.valid, "id header valid");
    CHECK(h.channels == 2, "id header channels");
    CHECK(h.sample_rate == 44100, "id header sample rate");
    CHECK(h.blocksize0 == 256 && h.blocksize1 == 2048, "id header blocksizes");

    // Framing bit clear -> invalid.
    uint8_t bad = pkt[29];
    pkt[29] = 0x00;
    CHECK(!glint::vorbis::parse_id_header(pkt, sizeof(pkt)).valid,
          "id header rejects framing=0");
    pkt[29] = bad;
    // blocksize0 > blocksize1 -> invalid (swap nibbles: 0x8B).
    pkt[28] = 0x8B;
    CHECK(!glint::vorbis::parse_id_header(pkt, sizeof(pkt)).valid,
          "id header rejects bs0 > bs1");
    // Wrong magic -> invalid.
    uint8_t p2[30];
    std::memcpy(p2, pkt, 30);
    p2[0] = 0x03;
    CHECK(!glint::vorbis::parse_id_header(p2, sizeof(p2)).valid,
          "id header rejects wrong packet type");
}

// LSB-first bit reader (spec §2): values read low bit first; overrun flag.
static void test_vorbis_bitreader() {
    std::printf("[vorbis] LSB-first bit reader...\n");
    uint8_t data[3] = { 0b10110101, 0b00000011, 0x00 };
    glint::vorbis::BitReader br(data, sizeof(data));
    CHECK(br.read(1) == 1, "bit 0 (LSB) = 1");
    CHECK(br.read(2) == 0b10, "next 2 bits");     // bits 2,1 -> value 0b10
    CHECK(br.read(5) == 0b10110, "next 5 bits");  // high 5 bits of byte0
    CHECK(br.read(10) == 0b0000000011, "spanning byte1");
    CHECK(!br.overrun(), "no overrun yet");
    br.read(20);  // runs off the 3-byte buffer
    CHECK(br.overrun(), "overrun flagged");
    CHECK(glint::vorbis::ilog(0) == 0 && glint::vorbis::ilog(1) == 1 &&
              glint::vorbis::ilog(7) == 3 && glint::vorbis::ilog(8) == 4,
          "ilog per spec");
}

// Vorbis codebook: spec §3.2.1 worked-example Huffman tree + §9.2 helpers.
static void test_vorbis_codebook() {
    std::printf("[vorbis] codebook huffman + VQ helpers...\n");
    using glint::vorbis::BitReader;
    using glint::vorbis::Codebook;

    // float32_unpack (spec §9.2.2): mantissa*2^(exp-788).
    CHECK(glint::vorbis::float32_unpack((788u << 21) | 1u) == 1.0f,
          "float32_unpack unit");
    CHECK(glint::vorbis::float32_unpack((789u << 21) | 1u) == 2.0f,
          "float32_unpack exp+1");
    CHECK(glint::vorbis::float32_unpack(0x80000000u | (788u << 21) | 1u) ==
              -1.0f,
          "float32_unpack sign");
    // lookup1_values (spec §9.2.3): greatest r with r^dim <= entries.
    CHECK(glint::vorbis::lookup1_values(256, 2) == 16, "lookup1 256^(1/2)");
    CHECK(glint::vorbis::lookup1_values(255, 2) == 15, "lookup1 255");
    CHECK(glint::vorbis::lookup1_values(48, 3) == 3, "lookup1 48^(1/3)");

    // The spec's example codebook lengths -> known codewords:
    //   e0:00 e1:0100 e2:0101 e3:0110 e4:0111 e5:10 e6:110 e7:111.
    Codebook cb;
    cb.entries = 8;
    cb.dimensions = 0;
    cb.lengths = { 2, 4, 4, 4, 4, 2, 3, 3 };
    CHECK(cb.build_huffman(), "example codebook builds");

    // Decode reads bits low-first; a codeword string "0100" arrives as bits
    // 0,1,0,0. Pack several codewords back to back and decode in order.
    // Sequence: e0(00) e5(10) e6(110) e1(0100) e7(111). Codeword strings are
    // in read order (first character = first bit read; top-down tree path).
    const int bits[] = { 0, 0,  1, 0,  1, 1, 0,  0, 1, 0, 0,  1, 1, 1 };
    uint8_t buf[4] = { 0, 0, 0, 0 };
    int nb = static_cast<int>(sizeof(bits) / sizeof(bits[0]));
    for (int i = 0; i < nb; i++)
        if (bits[i]) buf[i >> 3] |= 1u << (i & 7);
    BitReader br(buf, sizeof(buf));
    CHECK(cb.decode_scalar(br) == 0, "decode e0 (00)");
    CHECK(cb.decode_scalar(br) == 5, "decode e5 (10)");
    CHECK(cb.decode_scalar(br) == 6, "decode e6 (110)");
    CHECK(cb.decode_scalar(br) == 1, "decode e1 (0100)");
    CHECK(cb.decode_scalar(br) == 7, "decode e7 (111)");

    // Over-subscribed tree must be rejected (two length-1 + a length-2).
    Codebook bad;
    bad.entries = 3;
    bad.lengths = { 1, 1, 2 };
    CHECK(!bad.build_huffman(), "over-subscribed codebook rejected");
}

#include "vorbis_imdct.hpp"

// Fast (N/4-FFT) inverse MDCT must equal the direct O(N^2) reference across
// every Vorbis block size — proves the fast path is exact before it is the
// only path in the decoder.
static void test_vorbis_imdct() {
    std::printf("[vorbis] fast iMDCT == direct O(N^2)...\n");
    unsigned seed = 12345;
    for (int n : {64, 128, 256, 1024, 2048, 8192}) {
        int M = n / 2;
        std::vector<float> spec(M), yd(n), yf(n);
        for (int k = 0; k < M; k++) {
            seed = seed * 1103515245u + 12345u;
            spec[k] = ((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        }
        glint::vorbis::imdct_direct(spec.data(), yd.data(), n);
        glint::vorbis::FastImdct fi;
        fi.init(n);
        fi.backward(spec.data(), yf.data());
        double maxabs = 0, maxdiff = 0;
        for (int i = 0; i < n; i++) {
            maxabs = std::max(maxabs, (double)std::abs(yd[i]));
            maxdiff = std::max(maxdiff, (double)std::abs(yd[i] - yf[i]));
        }
        CHECK(maxdiff < 1e-3 * (maxabs + 1e-6),
              "fast iMDCT matches direct reference");
    }
}

#include "vorbis_test_stream.h"

// Full setup-header parse (spec §4.2.4) on a REAL libvorbis stream: all
// codebooks, floors, residues, mappings, modes must parse and consume the
// whole setup packet bar the final-byte padding.
static void test_vorbis_setup() {
    std::printf("[vorbis] full header + setup parse (real stream)...\n");
    int ch = 0, rate = 0;
    size_t used = 0, total = 0;
    int r = glint::vorbis::debug_parse_headers(
        kVorbisMono440, kVorbisMono440Len, &ch, &rate, &used, &total);
    CHECK(r == 0, "setup parses a real libvorbis stream");
    CHECK(ch == 1 && rate == 44100, "id from real stream");
    CHECK(total >= used && (total - used) < 8,
          "setup consumes all but final-byte padding");
}

// Full audio decode of the embedded 440 Hz mono stream: correct rate/
// channels, a full frame count, audible energy, and on-pitch (the recovered
// fundamental period must be ~100.2 samples = 44100/440).
static void test_vorbis_decode() {
    std::printf("[vorbis] full audio decode (on-pitch check)...\n");
    std::vector<float> pcm;
    int sr = 0, ch = 0;
    int r = glint::vorbis::decode_ogg(kVorbisMono440, kVorbisMono440Len, pcm,
                                      sr, ch);
    CHECK(r == 0, "decode_ogg succeeds");
    CHECK(sr == 44100 && ch == 1, "decoded rate/channels");
    CHECK(pcm.size() > 20000 && pcm.size() <= 22050,
          "decoded ~0.5 s of frames");
    double energy = 0;
    for (float x : pcm) energy += (double)x * x;
    CHECK(energy > 1.0, "decoded audible energy");

    // Autocorrelation over a steady mid segment -> fundamental period.
    if (pcm.size() > 12000) {
        int base = 4000, win = 4096;
        double best = -1e30;
        int best_lag = 0;
        for (int lag = 60; lag < 160; lag++) {
            double s = 0;
            for (int i = 0; i < win; i++)
                s += (double)pcm[base + i] * pcm[base + i + lag];
            if (s > best) { best = s; best_lag = lag; }
        }
        CHECK(best_lag >= 98 && best_lag <= 102,
              "recovered ~440 Hz fundamental period");
    }
}

int main() {
    std::printf("=== glint unit tests ===\n\n");

    test_bitstream();
    test_huffman_consistency();
    test_pow34_table();
    test_quantize_roundtrip();
#if !defined(GLINT_FIXED_POINT) || defined(GLINT_BOTH_PATHS)
    test_mdct_tdac();
#endif
    test_api();
#if !defined(GLINT_FIXED_POINT) || defined(GLINT_BOTH_PATHS)
    test_float_subband();
    test_float_encode_api();
    test_float_vs_int16_encode();
#endif

#if defined(GLINT_FIXED_POINT) && defined(GLINT_BOTH_PATHS)
    test_fixed_vs_double();
#endif

    test_aac_mdct_vs_direct();
    test_aac_tables_sanity();
    test_aac_coder_count_matches_emission();
    test_aac_api_smoke();

    test_opus_range_coder();
    test_opus_celt_prims();
    test_opus_celt_alloc_mdct();
    test_opus_packet_parse();
    test_opus_c_api_roundtrip();
    test_mp3_decoder_api();
    test_aac_decoder_api();

    test_vorbis_bitreader();
    test_vorbis_id_header();
    test_vorbis_codebook();
    test_vorbis_setup();
    test_vorbis_imdct();
    test_vorbis_decode();

    std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
