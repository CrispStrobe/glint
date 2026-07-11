// glint CLI audio pipeline: WAV/raw + MP3/AAC/Opus decode & encode,
// resample and gain, over a universal interleaved-float representation.
// MIT License - Clean-room implementation.
#ifndef GLINT_CLI_AUDIO_IO_HPP
#define GLINT_CLI_AUDIO_IO_HPP

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "glint/glint.h"
#include "opus_decoder.hpp"
#include "opus_celt_encoder.hpp"
#include "opus_ogg.hpp"
#include "resample.hpp"

namespace glint_cli {

// WAV format tags
static constexpr uint16_t WAVE_FORMAT_PCM        = 0x0001;
static constexpr uint16_t WAVE_FORMAT_IEEE_FLOAT  = 0x0003;
static constexpr uint16_t WAVE_FORMAT_ALAW        = 0x0006;
static constexpr uint16_t WAVE_FORMAT_MULAW       = 0x0007;
static constexpr uint16_t WAVE_FORMAT_EXTENSIBLE   = 0xFFFE;

// ITU-T G.711 A-law decode table (256 entries)
static constexpr int16_t alaw_table[256] = {
    -5504, -5248, -6016, -5760, -4480, -4224, -4992, -4736,
    -7552, -7296, -8064, -7808, -6528, -6272, -7040, -6784,
    -2752, -2624, -3008, -2880, -2240, -2112, -2496, -2368,
    -3776, -3648, -4032, -3904, -3264, -3136, -3520, -3392,
    -22016,-20992,-24064,-23040,-17920,-16896,-19968,-18944,
    -30208,-29184,-32256,-31232,-26112,-25088,-28160,-27136,
    -11008,-10496,-12032,-11520, -8960, -8448, -9984, -9472,
    -15104,-14592,-16128,-15616,-13056,-12544,-14080,-13568,
      -344,  -328,  -376,  -360,  -280,  -264,  -312,  -296,
      -472,  -456,  -504,  -488,  -408,  -392,  -440,  -424,
      -88,   -72,  -120,  -104,   -24,    -8,   -56,   -40,
     -216,  -200,  -248,  -232,  -152,  -136,  -184,  -168,
    -1376, -1312, -1504, -1440, -1120, -1056, -1248, -1184,
    -1888, -1824, -2016, -1952, -1632, -1568, -1760, -1696,
     -688,  -656,  -752,  -720,  -560,  -528,  -624,  -592,
     -944,  -912, -1008,  -976,  -816,  -784,  -880,  -848,
     5504,  5248,  6016,  5760,  4480,  4224,  4992,  4736,
     7552,  7296,  8064,  7808,  6528,  6272,  7040,  6784,
     2752,  2624,  3008,  2880,  2240,  2112,  2496,  2368,
     3776,  3648,  4032,  3904,  3264,  3136,  3520,  3392,
    22016, 20992, 24064, 23040, 17920, 16896, 19968, 18944,
    30208, 29184, 32256, 31232, 26112, 25088, 28160, 27136,
    11008, 10496, 12032, 11520,  8960,  8448,  9984,  9472,
    15104, 14592, 16128, 15616, 13056, 12544, 14080, 13568,
      344,   328,   376,   360,   280,   264,   312,   296,
      472,   456,   504,   488,   408,   392,   440,   424,
       88,    72,   120,   104,    24,     8,    56,    40,
      216,   200,   248,   232,   152,   136,   184,   168,
     1376,  1312,  1504,  1440,  1120,  1056,  1248,  1184,
     1888,  1824,  2016,  1952,  1632,  1568,  1760,  1696,
      688,   656,   752,   720,   560,   528,   624,   592,
      944,   912,  1008,   976,   816,   784,   880,   848,
};

// ITU-T G.711 mu-law decode table (256 entries)
static constexpr int16_t ulaw_table[256] = {
    -32124,-31100,-30076,-29052,-28028,-27004,-25980,-24956,
    -23932,-22908,-21884,-20860,-19836,-18812,-17788,-16764,
    -15996,-15484,-14972,-14460,-13948,-13436,-12924,-12412,
    -11900,-11388,-10876,-10364, -9852, -9340, -8828, -8316,
     -7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140,
     -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
     -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004,
     -2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980,
     -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
     -1372, -1308, -1244, -1180, -1116, -1052,  -988,  -924,
      -876,  -844,  -812,  -780,  -748,  -716,  -684,  -652,
      -620,  -588,  -556,  -524,  -492,  -460,  -428,  -396,
      -372,  -356,  -340,  -324,  -308,  -292,  -276,  -260,
      -244,  -228,  -212,  -196,  -180,  -164,  -148,  -132,
      -120,  -112,  -104,   -96,   -88,   -80,   -72,   -64,
       -56,   -48,   -40,   -32,   -24,   -16,    -8,     0,
     32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
     23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
     15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412,
     11900, 11388, 10876, 10364,  9852,  9340,  8828,  8316,
      7932,  7676,  7420,  7164,  6908,  6652,  6396,  6140,
      5884,  5628,  5372,  5116,  4860,  4604,  4348,  4092,
      3900,  3772,  3644,  3516,  3388,  3260,  3132,  3004,
      2876,  2748,  2620,  2492,  2364,  2236,  2108,  1980,
      1884,  1820,  1756,  1692,  1628,  1564,  1500,  1436,
      1372,  1308,  1244,  1180,  1116,  1052,   988,   924,
       876,   844,   812,   780,   748,   716,   684,   652,
       620,   588,   556,   524,   492,   460,   428,   396,
       372,   356,   340,   324,   308,   292,   276,   260,
       244,   228,   212,   196,   180,   164,   148,   132,
       120,   112,   104,    96,    88,    80,    72,    64,
        56,    48,    40,    32,    24,    16,     8,     0,
};

// WAV file header structures
#pragma pack(push, 1)
struct RiffHeader {
    char riff_id[4];      // "RIFF"
    uint32_t file_size;
    char wave_id[4];      // "WAVE"
};

struct ChunkHeader {
    char chunk_id[4];
    uint32_t chunk_size;
};

struct FmtChunk {
    uint16_t audio_format;  // 1 = PCM, 3 = float, 6 = A-law, 7 = mu-law
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
};

// Extended format chunk fields (for WAVE_FORMAT_EXTENSIBLE)
struct FmtExtension {
    uint16_t cb_size;           // Size of extension (22 for extensible)
    uint16_t valid_bits;        // Valid bits per sample
    uint32_t channel_mask;      // Speaker position mask
    uint8_t  sub_format[16];    // SubFormat GUID
};
#pragma pack(pop)

// Known SubFormat GUIDs (first 2 bytes encode the format tag)
static const uint8_t KSDATAFORMAT_SUBTYPE_PCM[16] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
};
static const uint8_t KSDATAFORMAT_SUBTYPE_IEEE_FLOAT[16] = {
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
};
static const uint8_t KSDATAFORMAT_SUBTYPE_ALAW[16] = {
    0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
};
static const uint8_t KSDATAFORMAT_SUBTYPE_MULAW[16] = {
    0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71
};

// Interleaved float PCM (+-1.0) with rate/channels — the pipeline's
// universal representation.
struct Audio {
    std::vector<float> pcm;
    int sr = 0;
    int ch = 0;
    long frames() const { return ch ? (long)(pcm.size() / ch) : 0; }
};

enum class Fmt { Unknown, Wav, Raw, Mp3, Aac, Opus };

inline bool ends_with(const std::string& s, const char* suf) {
    size_t n = std::strlen(suf);
    return s.size() >= n &&
           strcasecmp(s.c_str() + s.size() - n, suf) == 0;
}

inline Fmt fmt_from_ext(const std::string& path) {
    if (ends_with(path, ".wav")) return Fmt::Wav;
    if (ends_with(path, ".mp3")) return Fmt::Mp3;
    if (ends_with(path, ".aac")) return Fmt::Aac;
    if (ends_with(path, ".opus") || ends_with(path, ".ogg"))
        return Fmt::Opus;
    if (ends_with(path, ".raw") || ends_with(path, ".pcm"))
        return Fmt::Raw;
    return Fmt::Unknown;
}

// Detect an input by magic bytes when the extension is ambiguous.
inline Fmt fmt_from_magic(const uint8_t* d, size_t n) {
    if (n >= 4 && !std::memcmp(d, "RIFF", 4)) return Fmt::Wav;
    if (n >= 4 && !std::memcmp(d, "OggS", 4)) return Fmt::Opus;
    if (n >= 3 && !std::memcmp(d, "ID3", 3)) return Fmt::Mp3;
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xF6) == 0xF0) return Fmt::Aac;
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0) return Fmt::Mp3;
    return Fmt::Unknown;
}

// ---- byte I/O (path or "-" for stdin/stdout) ----
inline std::vector<uint8_t> read_all(const std::string& path) {
    std::vector<uint8_t> out;
    FILE* f = path == "-" ? stdin : std::fopen(path.c_str(), "rb");
    if (!f) return out;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
        out.insert(out.end(), buf, buf + n);
    if (f != stdin) std::fclose(f);
    return out;
}

inline bool write_all(const std::string& path, const uint8_t* d, size_t n) {
    FILE* f = path == "-" ? stdout : std::fopen(path.c_str(), "wb");
    if (!f) return false;
    size_t w = std::fwrite(d, 1, n, f);
    if (f != stdout) std::fclose(f);
    return w == n;
}

// ---- WAV parse (buffer) -> float Audio ----
inline float sample_to_float(const uint8_t* d, uint16_t fmt, int bps) {
    switch (fmt) {
    case WAVE_FORMAT_PCM:
        switch (bps) {
        case 8:
            return (static_cast<int>(d[0]) - 128) / 128.0f;
        case 16: {
            int16_t s;
            std::memcpy(&s, d, 2);
            return s / 32768.0f;
        }
        case 24: {
            int32_t v = d[0] | (d[1] << 8) | (d[2] << 16);
            if (v & 0x800000) v |= 0xFF000000;
            return v / 8388608.0f;
        }
        case 32: {
            int32_t v;
            std::memcpy(&v, d, 4);
            return static_cast<float>(v / 2147483648.0);
        }
        }
        return 0;
    case WAVE_FORMAT_IEEE_FLOAT:
        if (bps == 32) {
            float f;
            std::memcpy(&f, d, 4);
            return f;
        } else if (bps == 64) {
            double g;
            std::memcpy(&g, d, 8);
            return static_cast<float>(g);
        }
        return 0;
    case WAVE_FORMAT_ALAW:
        return alaw_table[d[0]] / 32768.0f;
    case WAVE_FORMAT_MULAW:
        return ulaw_table[d[0]] / 32768.0f;
    }
    return 0;
}

inline uint32_t rd32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t)p[3] << 24;
}
inline uint16_t rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }

inline bool parse_wav(const uint8_t* d, size_t n, Audio& a,
                      std::string& err) {
    if (n < 12 || std::memcmp(d, "RIFF", 4) || std::memcmp(d + 8, "WAVE", 4)) {
        err = "not a RIFF/WAVE file";
        return false;
    }
    size_t off = 12;
    uint16_t fmt = 0;
    int bps = 0, ch = 0, sr = 0;
    const uint8_t* data = nullptr;
    size_t data_len = 0;
    bool have_fmt = false;
    while (off + 8 <= n) {
        uint32_t csz = rd32(d + off + 4);
        const uint8_t* body = d + off + 8;
        if (!std::memcmp(d + off, "fmt ", 4) && off + 8 + 16 <= n) {
            fmt = rd16(body);
            ch = rd16(body + 2);
            sr = rd32(body + 4);
            bps = rd16(body + 14);
            if (fmt == WAVE_FORMAT_EXTENSIBLE && csz >= 40) {
                const uint8_t* sub = body + 24;  // SubFormat GUID
                if (!std::memcmp(sub, KSDATAFORMAT_SUBTYPE_PCM, 16))
                    fmt = WAVE_FORMAT_PCM;
                else if (!std::memcmp(sub, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 16))
                    fmt = WAVE_FORMAT_IEEE_FLOAT;
                else if (!std::memcmp(sub, KSDATAFORMAT_SUBTYPE_ALAW, 16))
                    fmt = WAVE_FORMAT_ALAW;
                else if (!std::memcmp(sub, KSDATAFORMAT_SUBTYPE_MULAW, 16))
                    fmt = WAVE_FORMAT_MULAW;
                int vbits = rd16(body + 18);
                if (vbits > 0) bps = vbits;
            }
            have_fmt = true;
        } else if (!std::memcmp(d + off, "data", 4)) {
            data = body;
            data_len = csz;
            if (off + 8 + data_len > n) data_len = n - (off + 8);
        }
        off += 8 + csz + (csz & 1);  // chunks are word-aligned
    }
    if (!have_fmt || !data || ch < 1 || sr < 1) {
        err = "missing fmt/data or bad parameters";
        return false;
    }
    int bytes = bps / 8;
    int stride = bytes * ch;
    if (stride <= 0) {
        err = "bad sample size";
        return false;
    }
    long frames = static_cast<long>(data_len / stride);
    a.sr = sr;
    a.ch = ch;
    a.pcm.resize(static_cast<size_t>(frames) * ch);
    for (long i = 0; i < frames; i++)
        for (int c = 0; c < ch; c++)
            a.pcm[i * ch + c] =
                sample_to_float(data + i * stride + c * bytes, fmt, bps);
    return true;
}

inline bool parse_raw(const uint8_t* d, size_t n, int sr, int ch, int bits,
                      Audio& a) {
    int bytes = bits / 8, stride = bytes * ch;
    if (stride <= 0) return false;
    long frames = static_cast<long>(n / stride);
    a.sr = sr;
    a.ch = ch;
    a.pcm.resize(static_cast<size_t>(frames) * ch);
    for (long i = 0; i < frames; i++)
        for (int c = 0; c < ch; c++)
            a.pcm[i * ch + c] = sample_to_float(
                d + i * stride + c * bytes, WAVE_FORMAT_PCM, bits);
    return true;
}

// ---- WAV write (16-bit PCM by default; 32-bit float if bits==32f) ----
inline void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(x); v.push_back(x >> 8);
    v.push_back(x >> 16); v.push_back(x >> 24);
}
inline void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x); v.push_back(x >> 8);
}

inline std::vector<uint8_t> write_wav(const Audio& a, int bits,
                                      bool is_float) {
    std::vector<uint8_t> v;
    int bytes = bits / 8;
    uint32_t data_bytes =
        static_cast<uint32_t>(a.pcm.size()) * bytes;
    uint16_t tag = is_float ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    v.insert(v.end(), {'R', 'I', 'F', 'F'});
    put32(v, 36 + data_bytes);
    v.insert(v.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(v, 16);
    put16(v, tag);
    put16(v, static_cast<uint16_t>(a.ch));
    put32(v, static_cast<uint32_t>(a.sr));
    put32(v, static_cast<uint32_t>(a.sr) * a.ch * bytes);  // byte rate
    put16(v, static_cast<uint16_t>(a.ch * bytes));         // block align
    put16(v, static_cast<uint16_t>(bits));
    v.insert(v.end(), {'d', 'a', 't', 'a'});
    put32(v, data_bytes);
    for (float f : a.pcm) {
        if (is_float) {
            uint8_t b[4];
            std::memcpy(b, &f, 4);
            v.insert(v.end(), b, b + 4);
        } else {
            float s = f * 32768.0f;
            int iv = static_cast<int>(s > 0 ? s + 0.5f : s - 0.5f);
            if (iv > 32767) iv = 32767;
            if (iv < -32768) iv = -32768;
            put16(v, static_cast<uint16_t>(static_cast<int16_t>(iv)));
        }
    }
    return v;
}

// ---- decode ----
inline bool decode_mp3(const uint8_t* d, size_t n, Audio& a) {
    size_t off = 0;
    if (n > 10 && !std::memcmp(d, "ID3", 3))
        off = 10 + ((d[6] & 0x7F) << 21 | (d[7] & 0x7F) << 14 |
                    (d[8] & 0x7F) << 7 | (d[9] & 0x7F));
    glint_mp3_dec_t dec = glint_mp3_dec_create();
    if (!dec) return false;
    float pcm[2 * 1152];
    glint_dec_frame_info fi;
    a.pcm.clear();
    a.sr = a.ch = 0;
    while (off + 4 <= n) {
        if (glint_mp3_frame_info(d + off, (int)(n - off), &fi) < 0) {
            off++;
            continue;
        }
        if (fi.frame_bytes <= 0 || off + (size_t)fi.frame_bytes > n) break;
        int s = glint_mp3_decode(dec, d + off, (int)(n - off), pcm, &fi);
        if (s > 0) {
            a.sr = fi.sample_rate;
            a.ch = fi.channels;
            a.pcm.insert(a.pcm.end(), pcm, pcm + (size_t)s * fi.channels);
        }
        off += fi.frame_bytes;
    }
    glint_mp3_dec_destroy(dec);
    return a.ch > 0;
}

inline bool decode_aac(const uint8_t* d, size_t n, Audio& a) {
    glint_aac_dec_t dec = glint_aac_dec_create();
    if (!dec) return false;
    float pcm[2 * 1024];
    glint_dec_frame_info fi;
    size_t off = 0;
    a.pcm.clear();
    a.sr = a.ch = 0;
    while (off + 7 <= n) {
        if (glint_aac_frame_info(d + off, (int)(n - off), &fi) < 0) {
            off++;
            continue;
        }
        if (fi.frame_bytes <= 0 || off + (size_t)fi.frame_bytes > n) break;
        int s = glint_aac_decode(dec, d + off, (int)(n - off), pcm, &fi);
        if (s > 0) {
            a.sr = fi.sample_rate;
            a.ch = fi.channels;
            a.pcm.insert(a.pcm.end(), pcm, pcm + (size_t)s * fi.channels);
        }
        off += fi.frame_bytes;
    }
    glint_aac_dec_destroy(dec);
    return a.ch > 0;
}

inline bool decode_opus(const uint8_t* d, size_t n, Audio& a) {
    glint::opus::OggOpusReader r;
    if (r.parse(d, n) != 0) return false;
    int ch = r.head().channels;
    if (ch < 1 || ch > 2) return false;
    glint::opus::OpusDecoder dec;
    dec.init(ch);
    std::vector<float> pcm(2 * 5760);
    a.ch = ch;
    a.sr = 48000;
    a.pcm.clear();
    for (int i = 0; i < r.packet_count(); i++) {
        const auto& p = r.packet(i);
        int s = dec.decode(p.data(), (int)p.size(), pcm.data(), 5760);
        if (s > 0)
            a.pcm.insert(a.pcm.end(), pcm.data(),
                         pcm.data() + (size_t)s * ch);
    }
    // Apply the OpusHead edit list: drop pre-skip, trim to granule end.
    int pre = r.head().pre_skip;
    double gain = r.output_gain();
    if (gain != 1.0)
        for (auto& x : a.pcm) x = static_cast<float>(x * gain);
    if (pre > 0 && (long)a.pcm.size() >= (long)pre * ch)
        a.pcm.erase(a.pcm.begin(), a.pcm.begin() + (size_t)pre * ch);
    int64_t total = r.total_samples();
    if (total >= 0 && (long)a.pcm.size() > total * ch)
        a.pcm.resize((size_t)total * ch);
    return true;
}

// ---- process: gain, normalize, resample ----
inline void apply_gain(Audio& a, double db) {
    if (db == 0.0) return;
    double g = std::pow(10.0, db / 20.0);
    for (auto& x : a.pcm) x = static_cast<float>(x * g);
}

inline void normalize(Audio& a, double target_dbfs) {
    double peak = 0;
    for (float x : a.pcm) peak = std::max(peak, (double)std::fabs(x));
    if (peak <= 0) return;
    double target = std::pow(10.0, target_dbfs / 20.0);
    double g = target / peak;
    for (auto& x : a.pcm) x = static_cast<float>(x * g);
}

inline Audio to_rate(const Audio& a, int sr) {
    if (sr == a.sr || a.ch == 0) return a;
    Audio out;
    out.sr = sr;
    out.ch = a.ch;
    int nf = 0;
    out.pcm = glint::resample(a.pcm.data(), (int)a.frames(), a.ch, a.sr,
                              sr, &nf);
    return out;
}

// ---- encode ----
inline std::vector<uint8_t> encode_mp3(const Audio& a, int bitrate,
                                       int vbr_q, int mode, int quality) {
    std::vector<uint8_t> out;
    glint_config cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = a.sr;
    cfg.num_channels = a.ch;
    cfg.mode = mode >= 0 ? (glint_mode)mode
                         : (a.ch == 1 ? GLINT_MONO : GLINT_JOINT);
    cfg.bitrate = bitrate;
    cfg.quality = (glint_quality)quality;
    if (vbr_q >= 0) {
        cfg.vbr = GLINT_VBR_ON;
        cfg.vbr_quality = vbr_q;
    }
    glint_t enc = glint_create(&cfg);
    if (!enc) return out;
    int spf = glint_samples_per_frame(enc);
    std::vector<float> l(spf), r(spf);
    const float* ch[2] = { l.data(), r.data() };
    long total = a.frames();
    for (long p = 0; p < total; p += spf) {
        int got = (int)std::min<long>(spf, total - p);
        for (int i = 0; i < spf; i++) {
            if (i < got) {
                l[i] = a.pcm[(p + i) * a.ch];
                r[i] = a.ch > 1 ? a.pcm[(p + i) * a.ch + 1] : l[i];
            } else {
                l[i] = r[i] = 0;
            }
        }
        int n = 0;
        const uint8_t* fr = glint_encode_float(enc, ch, &n);
        if (fr && n > 0) out.insert(out.end(), fr, fr + n);
    }
    int n = 0;
    const uint8_t* fl = glint_flush(enc, &n);
    if (fl && n > 0) out.insert(out.end(), fl, fl + n);
    glint_destroy(enc);
    return out;
}

inline std::vector<uint8_t> encode_aac(const Audio& a, int bitrate,
                                       int vbr_q, int mono, int quality) {
    std::vector<uint8_t> out;
    glint_aac_config cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.sample_rate = a.sr;
    cfg.num_channels = mono ? 1 : a.ch;
    cfg.bitrate = bitrate;
    cfg.quality = (glint_quality)quality;
    if (vbr_q >= 0) {
        cfg.vbr = 1;
        cfg.vbr_quality = vbr_q;
    }
    glint_aac_t enc = glint_aac_create(&cfg);
    if (!enc) return out;
    int spf = glint_aac_samples_per_frame(enc);
    int ec = cfg.num_channels;
    std::vector<float> l(spf), r(spf);
    const float* ch[2] = { l.data(), r.data() };
    long total = a.frames();
    for (long p = 0; p < total; p += spf) {
        int got = (int)std::min<long>(spf, total - p);
        for (int i = 0; i < spf; i++) {
            float sl = 0, sr = 0;
            if (i < got) {
                sl = a.pcm[(p + i) * a.ch];
                sr = a.ch > 1 ? a.pcm[(p + i) * a.ch + 1] : sl;
            }
            if (ec == 1) {
                l[i] = a.ch > 1 ? 0.5f * (sl + sr) : sl;
            } else {
                l[i] = sl;
                r[i] = sr;
            }
        }
        int n = 0;
        const uint8_t* fr = glint_aac_encode_float(enc, ch, &n);
        if (fr && n > 0) out.insert(out.end(), fr, fr + n);
    }
    int n = 0;
    const uint8_t* fl = glint_aac_flush(enc, &n);
    if (fl && n > 0) out.insert(out.end(), fl, fl + n);
    glint_aac_destroy(enc);
    return out;
}

// Opus: 48 kHz only. The caller resamples a.sr to 48000 first.
inline std::vector<uint8_t> encode_opus(const Audio& a, int bitrate,
                                        bool vbr) {
    std::vector<uint8_t> out;
    if (a.sr != 48000 || a.ch < 1 || a.ch > 2) return out;
    glint::opus::CeltEncoder enc;
    enc.init(a.ch);
    if (vbr) enc.set_vbr(bitrate);
    glint::opus::OggOpusWriter w;
    w.begin(a.ch, 120, 48000);  // pre-skip 120 (one CELT overlap)
    const int frame = 960;  // 20 ms
    uint8_t pkt[1500];
    std::vector<float> buf(frame * a.ch);
    long total = a.frames();
    for (long p = 0; p < total; p += frame) {
        int got = (int)std::min<long>(frame, total - p);
        for (int i = 0; i < frame * a.ch; i++) buf[i] = 0;
        for (int i = 0; i < got * a.ch; i++) buf[i] = a.pcm[p * a.ch + i];
        int nb = vbr ? 1275 : bitrate * frame / 48000 / 8 - 1;
        if (nb < 2) nb = 2;
        if (nb > 1275) nb = 1275;
        pkt[0] = static_cast<uint8_t>((31 << 3) | ((a.ch == 2) << 2));
        int r = enc.encode_frame(buf.data(), frame, pkt + 1, nb);
        if (r < 0) continue;
        w.add_packet(pkt, 1 + r, frame);
    }
    const auto& bytes = w.finish();
    out.assign(bytes.begin(), bytes.end());
    return out;
}

}  // namespace glint_cli

#endif  // GLINT_CLI_AUDIO_IO_HPP
