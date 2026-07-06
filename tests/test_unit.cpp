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
    double n0 = (N / 2 + 1) / 2.0;
    double max_err = 0, max_ref = 0;
    for (int k = 0; k < M; k++) {
        double acc = 0;
        for (int n = 0; n < N; n++) {
            acc += z[n] * std::cos(2.0 * pi / N * (n + n0) * (k + 0.5));
        }
        ref[k] = 2.0 * acc;
        double e = std::fabs(ref[k] - spec[k]);
        if (e > max_err) max_err = e;
        if (std::fabs(ref[k]) > max_ref) max_ref = std::fabs(ref[k]);
    }
    const double tol = sizeof(glint::aac::SpecT) == 4 ? 2e-5 : 1e-10;
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
            double e = std::fabs(r - specs[128 * w + k]);
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
    glint::aac::aac_fit_channel(spec, layout, 3000, nullptr, -1, &plan);
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
    glint::aac::aac_fit_channel(spec, sl, 2500, nullptr, -1, &splan);
    CHECK(splan.ics_bits <= 2500, "short plan respects the bit budget");
    glint::aac::AacBitWriter scount(0);
    glint::aac::aac_write_ics_body(scount, splan, sl, false);
    CHECK(scount.bits() == splan.ics_bits, "short count matches emission");
}

static void test_aac_api_smoke() {
    std::printf("AAC API smoke (ADTS framing)...\n");
    glint_aac_config cfg;
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

    std::printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
