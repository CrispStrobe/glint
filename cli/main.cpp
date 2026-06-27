// glint - CLI frontend
// MIT License - Clean-room implementation
//
// Minimal WAV-to-MP3 encoder command line tool.
// Supports: PCM 8/16/24/32-bit, IEEE float 32/64-bit,
//           A-law, mu-law, and WAVE_FORMAT_EXTENSIBLE.

#include "glint/glint.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <chrono>

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

struct WavInfo {
    int sample_rate;
    int num_channels;
    int bits_per_sample;
    uint16_t audio_format;  // resolved format (1, 3, 6, or 7)
    long data_offset;
    uint32_t data_size;
};

// Convert a single sample from raw bytes to int16_t
static int16_t convert_sample(const uint8_t* data, uint16_t format, int bps) {
    switch (format) {
    case WAVE_FORMAT_PCM:
        switch (bps) {
        case 8:
            return static_cast<int16_t>((static_cast<int>(data[0]) - 128) << 8);
        case 16: {
            int16_t s;
            memcpy(&s, data, 2);
            return s;
        }
        case 24: {
            // Read 3 bytes little-endian, sign-extend to int32, shift >> 8
            int32_t val = static_cast<int32_t>(data[0])
                        | (static_cast<int32_t>(data[1]) << 8)
                        | (static_cast<int32_t>(data[2]) << 16);
            // Sign extend from 24 bits
            if (val & 0x800000) val |= 0xFF000000;
            return static_cast<int16_t>(val >> 8);
        }
        case 32: {
            int32_t val;
            memcpy(&val, data, 4);
            return static_cast<int16_t>(val >> 16);
        }
        default:
            return 0;
        }

    case WAVE_FORMAT_IEEE_FLOAT:
        if (bps == 32) {
            float f;
            memcpy(&f, data, 4);
            float scaled = f * 32767.0f;
            if (scaled > 32767.0f) scaled = 32767.0f;
            if (scaled < -32768.0f) scaled = -32768.0f;
            return static_cast<int16_t>(scaled);
        } else if (bps == 64) {
            double d;
            memcpy(&d, data, 8);
            double scaled = d * 32767.0;
            if (scaled > 32767.0) scaled = 32767.0;
            if (scaled < -32768.0) scaled = -32768.0;
            return static_cast<int16_t>(scaled);
        }
        return 0;

    case WAVE_FORMAT_ALAW:
        return alaw_table[data[0]];

    case WAVE_FORMAT_MULAW:
        return ulaw_table[data[0]];

    default:
        return 0;
    }
}

static bool read_wav_header(FILE* f, WavInfo* info) {
    RiffHeader riff;
    if (fread(&riff, sizeof(riff), 1, f) != 1) return false;
    if (memcmp(riff.riff_id, "RIFF", 4) != 0) return false;
    if (memcmp(riff.wave_id, "WAVE", 4) != 0) return false;

    bool found_fmt = false;
    bool found_data = false;

    while (!found_data) {
        ChunkHeader ch;
        if (fread(&ch, sizeof(ch), 1, f) != 1) break;

        if (memcmp(ch.chunk_id, "fmt ", 4) == 0) {
            long fmt_start = ftell(f);
            FmtChunk fmt;
            if (fread(&fmt, sizeof(fmt), 1, f) != 1) return false;

            uint16_t resolved_format = fmt.audio_format;
            int bps = fmt.bits_per_sample;

            if (fmt.audio_format == WAVE_FORMAT_EXTENSIBLE) {
                // Read the extension
                FmtExtension ext;
                if (ch.chunk_size < sizeof(FmtChunk) + sizeof(FmtExtension)) {
                    fprintf(stderr, "Error: extensible WAV header too short\n");
                    return false;
                }
                if (fread(&ext, sizeof(ext), 1, f) != 1) return false;

                // Resolve format from SubFormat GUID
                if (memcmp(ext.sub_format, KSDATAFORMAT_SUBTYPE_PCM, 16) == 0) {
                    resolved_format = WAVE_FORMAT_PCM;
                } else if (memcmp(ext.sub_format, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 16) == 0) {
                    resolved_format = WAVE_FORMAT_IEEE_FLOAT;
                } else if (memcmp(ext.sub_format, KSDATAFORMAT_SUBTYPE_ALAW, 16) == 0) {
                    resolved_format = WAVE_FORMAT_ALAW;
                } else if (memcmp(ext.sub_format, KSDATAFORMAT_SUBTYPE_MULAW, 16) == 0) {
                    resolved_format = WAVE_FORMAT_MULAW;
                } else {
                    fprintf(stderr, "Error: unsupported WAVE_FORMAT_EXTENSIBLE SubFormat\n");
                    return false;
                }

                if (ext.valid_bits > 0) bps = ext.valid_bits;
            }

            // Validate format/bps combinations
            bool valid = false;
            switch (resolved_format) {
            case WAVE_FORMAT_PCM:
                valid = (bps == 8 || bps == 16 || bps == 24 || bps == 32);
                break;
            case WAVE_FORMAT_IEEE_FLOAT:
                valid = (bps == 32 || bps == 64);
                break;
            case WAVE_FORMAT_ALAW:
            case WAVE_FORMAT_MULAW:
                valid = (bps == 8);
                break;
            default:
                fprintf(stderr, "Error: unsupported WAV format tag 0x%04X\n", fmt.audio_format);
                return false;
            }

            if (!valid) {
                fprintf(stderr, "Error: unsupported bit depth %d for format 0x%04X\n",
                        bps, resolved_format);
                return false;
            }

            info->sample_rate = fmt.sample_rate;
            info->num_channels = fmt.num_channels;
            info->bits_per_sample = bps;
            info->audio_format = resolved_format;
            found_fmt = true;

            // Skip any remaining fmt bytes
            long consumed = ftell(f) - fmt_start;
            long remaining = static_cast<long>(ch.chunk_size) - consumed;
            if (remaining > 0) fseek(f, remaining, SEEK_CUR);
        } else if (memcmp(ch.chunk_id, "data", 4) == 0) {
            info->data_offset = ftell(f);
            info->data_size = ch.chunk_size;
            found_data = true;
        } else {
            // Skip unknown chunk
            fseek(f, ch.chunk_size, SEEK_CUR);
        }
    }

    return found_fmt && found_data;
}

static const char* format_name(uint16_t fmt, int bps) {
    switch (fmt) {
    case WAVE_FORMAT_PCM:
        if (bps == 8) return "8-bit unsigned PCM";
        if (bps == 16) return "16-bit signed PCM";
        if (bps == 24) return "24-bit signed PCM";
        if (bps == 32) return "32-bit signed PCM";
        return "PCM";
    case WAVE_FORMAT_IEEE_FLOAT:
        if (bps == 32) return "32-bit IEEE float";
        if (bps == 64) return "64-bit IEEE float";
        return "IEEE float";
    case WAVE_FORMAT_ALAW:  return "A-law";
    case WAVE_FORMAT_MULAW: return "mu-law";
    default: return "unknown";
    }
}

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [options] input.wav output.mp3\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -b BITRATE       Bitrate in kbps (default: 128)\n");
    fprintf(stderr, "  -V QUALITY       VBR quality 0-9 (0=best/largest, 9=worst/smallest)\n");
    fprintf(stderr, "  -m MODE          mono|stereo|joint (default: auto)\n");
#ifdef GLINT_BOTH_PATHS
    fprintf(stderr, "  -p PATH          double|fixed (default: fixed)\n");
#endif
    fprintf(stderr, "  -s SIMD          auto|avx|sse2|neon|none (default: auto)\n");
    fprintf(stderr, "  -q QUALITY       speed|normal|best (default: speed)\n");
    fprintf(stderr, "  -r RATE:CH:BITS  Raw PCM input (e.g., 44100:1:16)\n");
}

// Parse raw PCM spec: "RATE:CHANNELS:BITS"
static bool parse_raw_spec(const char* spec, int* rate, int* channels, int* bits) {
    char buf[128];
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char* p1 = strchr(buf, ':');
    if (!p1) return false;
    *p1++ = '\0';
    char* p2 = strchr(p1, ':');
    if (!p2) return false;
    *p2++ = '\0';

    *rate = atoi(buf);
    *channels = atoi(p1);
    *bits = atoi(p2);

    if (*rate <= 0 || *channels <= 0 || *channels > 2) return false;
    if (*bits != 8 && *bits != 16 && *bits != 24 && *bits != 32) return false;
    return true;
}

int main(int argc, char** argv) {
    int bitrate = 128;
    int vbr_quality = -1;  // -1 = not set (CBR mode)
    const char* mode_str = nullptr;
    const char* path_str = nullptr;
    const char* simd_str = nullptr;
    const char* quality_str = nullptr;
    const char* raw_spec = nullptr;
    const char* input_path = nullptr;
    const char* output_path = nullptr;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            bitrate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-V") == 0 && i + 1 < argc) {
            vbr_quality = atoi(argv[++i]);
            if (vbr_quality < 0 || vbr_quality > 9) {
                fprintf(stderr, "Error: VBR quality must be 0-9\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            mode_str = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            path_str = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            simd_str = argv[++i];
        } else if (strcmp(argv[i], "-q") == 0 && i + 1 < argc) {
            quality_str = argv[++i];
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            raw_spec = argv[++i];
        } else if (!input_path) {
            input_path = argv[i];
        } else if (!output_path) {
            output_path = argv[i];
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!input_path || !output_path) {
        print_usage(argv[0]);
        return 1;
    }

    // Open input file
    FILE* wav_file = fopen(input_path, "rb");
    if (!wav_file) {
        fprintf(stderr, "Error: cannot open input file '%s'\n", input_path);
        return 1;
    }

    WavInfo wav;
    memset(&wav, 0, sizeof(wav));

    if (raw_spec) {
        // Raw PCM mode: skip WAV header parsing
        int rate, channels, bits;
        if (!parse_raw_spec(raw_spec, &rate, &channels, &bits)) {
            fprintf(stderr, "Error: invalid raw spec '%s' (expected RATE:CHANNELS:BITS)\n", raw_spec);
            fclose(wav_file);
            return 1;
        }
        wav.sample_rate = rate;
        wav.num_channels = channels;
        wav.bits_per_sample = bits;
        wav.audio_format = WAVE_FORMAT_PCM;
        wav.data_offset = 0;

        // Get file size for data_size
        fseek(wav_file, 0, SEEK_END);
        long file_size = ftell(wav_file);
        fseek(wav_file, 0, SEEK_SET);
        wav.data_size = static_cast<uint32_t>(file_size);

        fprintf(stderr, "Input: %d Hz, %d channel(s), %s (raw PCM)\n",
                wav.sample_rate, wav.num_channels,
                format_name(wav.audio_format, wav.bits_per_sample));
    } else {
        // WAV file mode
        if (!read_wav_header(wav_file, &wav)) {
            fprintf(stderr, "Error: invalid WAV file\n");
            fclose(wav_file);
            return 1;
        }

        fprintf(stderr, "Input: %d Hz, %d channel(s), %s\n",
                wav.sample_rate, wav.num_channels,
                format_name(wav.audio_format, wav.bits_per_sample));
    }

    // Determine encoder mode
    glint_mode mode;
    if (mode_str) {
        if (strcmp(mode_str, "mono") == 0) {
            mode = GLINT_MONO;
        } else if (strcmp(mode_str, "stereo") == 0) {
            mode = GLINT_STEREO;
        } else if (strcmp(mode_str, "joint") == 0) {
            mode = GLINT_JOINT;
        } else {
            fprintf(stderr, "Error: invalid mode '%s'\n", mode_str);
            fclose(wav_file);
            return 1;
        }
    } else {
        mode = (wav.num_channels == 1) ? GLINT_MONO : GLINT_JOINT;
    }

    // Validate config (VBR uses 320 kbps internally)
    int check_bitrate = (vbr_quality >= 0) ? 320 : bitrate;
    if (glint_check_config(wav.sample_rate, check_bitrate) != 0) {
        fprintf(stderr, "Error: invalid sample rate (%d) or bitrate (%d kbps)\n",
                wav.sample_rate, check_bitrate);
        fclose(wav_file);
        return 1;
    }

    // If user requested mono but input is stereo, downmix to mono
    int enc_channels = wav.num_channels;
    if (mode == GLINT_MONO && wav.num_channels == 2) {
        enc_channels = 1;
    }

    // Create encoder
    glint_config cfg;
    cfg.sample_rate = wav.sample_rate;
    cfg.num_channels = enc_channels;
    cfg.mode = mode;
    cfg.bitrate = bitrate;
    cfg.path = GLINT_PATH_DEFAULT;
    if (path_str) {
        if (strcmp(path_str, "double") == 0 || strcmp(path_str, "d") == 0)
            cfg.path = GLINT_PATH_DOUBLE;
        else if (strcmp(path_str, "fixed") == 0 || strcmp(path_str, "f") == 0)
            cfg.path = GLINT_PATH_FIXED;
        else {
            fprintf(stderr, "Error: invalid path '%s' (use double or fixed)\n", path_str);
            fclose(wav_file);
            return 1;
        }
    }
    cfg.simd = GLINT_SIMD_AUTO;
    if (simd_str) {
        if (strcmp(simd_str, "auto") == 0) cfg.simd = GLINT_SIMD_AUTO;
        else if (strcmp(simd_str, "avx") == 0) cfg.simd = GLINT_SIMD_AVX;
        else if (strcmp(simd_str, "sse2") == 0 || strcmp(simd_str, "sse") == 0) cfg.simd = GLINT_SIMD_SSE2;
        else if (strcmp(simd_str, "neon") == 0) cfg.simd = GLINT_SIMD_NEON;
        else if (strcmp(simd_str, "none") == 0 || strcmp(simd_str, "scalar") == 0) cfg.simd = GLINT_SIMD_NONE;
        else {
            fprintf(stderr, "Error: invalid SIMD '%s' (use auto|avx|sse2|neon|none)\n", simd_str);
            fclose(wav_file);
            return 1;
        }
    }
    cfg.quality = GLINT_QUALITY_NORMAL;  // default: good quality at ~15% speed cost
    if (quality_str) {
        if (strcmp(quality_str, "speed") == 0) cfg.quality = GLINT_QUALITY_SPEED;
        else if (strcmp(quality_str, "normal") == 0) cfg.quality = GLINT_QUALITY_NORMAL;
        else if (strcmp(quality_str, "best") == 0) cfg.quality = GLINT_QUALITY_BEST;
        else {
            fprintf(stderr, "Error: invalid quality '%s' (use speed, normal, or best)\n", quality_str);
            fclose(wav_file);
            return 1;
        }
    }
    cfg.vbr = GLINT_VBR_OFF;
    cfg.vbr_quality = 0;
    if (vbr_quality >= 0) {
        cfg.vbr = GLINT_VBR_ON;
        cfg.vbr_quality = vbr_quality;
    }

    glint_t enc = glint_create(&cfg);
    if (!enc) {
        fprintf(stderr, "Error: failed to create encoder\n");
        fclose(wav_file);
        return 1;
    }

    // Open output file
    FILE* mp3_file = fopen(output_path, "wb");
    if (!mp3_file) {
        fprintf(stderr, "Error: cannot open output file '%s'\n", output_path);
        glint_destroy(enc);
        fclose(wav_file);
        return 1;
    }

    if (vbr_quality >= 0) {
        fprintf(stderr, "Encoding: VBR quality %d, %s\n", vbr_quality,
                mode == GLINT_MONO ? "mono" :
                mode == GLINT_JOINT ? "joint stereo" :
                mode == GLINT_STEREO ? "stereo" : "dual channel");
    } else {
        fprintf(stderr, "Encoding: %d kbps, %s\n", bitrate,
                mode == GLINT_MONO ? "mono" :
                mode == GLINT_JOINT ? "joint stereo" :
                mode == GLINT_STEREO ? "stereo" : "dual channel");
    }

    // Encode
    int samples_per_frame = glint_samples_per_frame(enc);
    int nch = wav.num_channels;
    int bytes_per_sample = wav.bits_per_sample / 8;
    int sample_stride = bytes_per_sample * nch;  // bytes per interleaved sample group

    // Allocate raw byte buffer for reading
    uint8_t* raw_buf = new uint8_t[samples_per_frame * sample_stride];
    int16_t* channel_buf[2];
    channel_buf[0] = new int16_t[samples_per_frame];
    channel_buf[1] = new int16_t[samples_per_frame];

    uint32_t total_samples = wav.data_size / sample_stride; // per-frame sample groups
    uint32_t samples_encoded = 0;
    uint64_t bytes_written = 0;

    fseek(wav_file, wav.data_offset, SEEK_SET);

    auto start_time = std::chrono::steady_clock::now();

    while (samples_encoded < total_samples) {
        int samples_to_read = samples_per_frame;
        if (samples_encoded + static_cast<uint32_t>(samples_to_read) > total_samples) {
            samples_to_read = static_cast<int>(total_samples - samples_encoded);
        }

        // Read raw interleaved bytes
        size_t read_count = fread(raw_buf, sample_stride, samples_to_read, wav_file);
        if (read_count == 0) break;
        int got = static_cast<int>(read_count);

        // Convert and deinterleave — fast path for 16-bit PCM (>99% of files)
        bool downmix = (enc_channels == 1 && nch == 2);
        if (wav.audio_format == WAVE_FORMAT_PCM && wav.bits_per_sample == 16 && !downmix) {
            const int16_t* pcm16 = reinterpret_cast<const int16_t*>(raw_buf);
            if (nch == 1) {
                std::memcpy(channel_buf[0], pcm16, got * sizeof(int16_t));
            } else {
                for (int i = 0; i < got; i++) {
                    channel_buf[0][i] = pcm16[i * 2];
                    channel_buf[1][i] = pcm16[i * 2 + 1];
                }
            }
        } else {
            for (int i = 0; i < got; i++) {
                if (downmix) {
                    int16_t l = convert_sample(raw_buf + i * sample_stride, wav.audio_format, wav.bits_per_sample);
                    int16_t r = convert_sample(raw_buf + i * sample_stride + bytes_per_sample, wav.audio_format, wav.bits_per_sample);
                    channel_buf[0][i] = static_cast<int16_t>((l + r) / 2);
                } else {
                    channel_buf[0][i] = convert_sample(raw_buf + i * sample_stride, wav.audio_format, wav.bits_per_sample);
                    if (nch > 1) {
                        channel_buf[1][i] = convert_sample(raw_buf + i * sample_stride + bytes_per_sample, wav.audio_format, wav.bits_per_sample);
                    }
                }
            }
        }
        // Zero-pad remainder
        for (int i = got; i < samples_per_frame; i++) {
            channel_buf[0][i] = 0;
            if (enc_channels > 1) channel_buf[1][i] = 0;
        }

        // Encode frame
        int out_size = 0;
        const int16_t* ch_ptrs[2] = { channel_buf[0], channel_buf[1] };
        const uint8_t* frame_data = glint_encode(enc, ch_ptrs, &out_size);

        if (frame_data && out_size > 0) {
            fwrite(frame_data, 1, out_size, mp3_file);
            bytes_written += out_size;
        }

        samples_encoded += got;
    }

    // Flush
    int flush_size = 0;
    const uint8_t* flush_data = glint_flush(enc, &flush_size);
    if (flush_data && flush_size > 0) {
        fwrite(flush_data, 1, flush_size, mp3_file);
        bytes_written += flush_size;
    }

    auto end_time = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(end_time - start_time).count();
    double audio_duration = static_cast<double>(samples_encoded) / wav.sample_rate;
    double speed = (elapsed > 0) ? audio_duration / elapsed : 0;

    fprintf(stderr, "Done: %u samples, %lu bytes written\n",
            samples_encoded, static_cast<unsigned long>(bytes_written));
    fprintf(stderr, "Speed: %.1fx realtime (%.2f sec audio in %.2f sec)\n",
            speed, audio_duration, elapsed);

    // Cleanup
    delete[] raw_buf;
    delete[] channel_buf[0];
    delete[] channel_buf[1];
    glint_destroy(enc);
    fclose(mp3_file);
    fclose(wav_file);

    return 0;
}
