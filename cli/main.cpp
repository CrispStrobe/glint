// glint - CLI frontend
// MIT License - Clean-room implementation
//
// Minimal WAV-to-MP3 encoder command line tool.

#include "glint/glint.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <chrono>

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
    uint16_t audio_format;  // 1 = PCM
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
};
#pragma pack(pop)

struct WavInfo {
    int sample_rate;
    int num_channels;
    int bits_per_sample;
    long data_offset;
    uint32_t data_size;
};

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
            FmtChunk fmt;
            if (fread(&fmt, sizeof(fmt), 1, f) != 1) return false;
            if (fmt.audio_format != 1) {
                fprintf(stderr, "Error: only PCM WAV files are supported\n");
                return false;
            }
            if (fmt.bits_per_sample != 16) {
                fprintf(stderr, "Error: only 16-bit WAV files are supported\n");
                return false;
            }
            info->sample_rate = fmt.sample_rate;
            info->num_channels = fmt.num_channels;
            info->bits_per_sample = fmt.bits_per_sample;
            found_fmt = true;
            // Skip any extra fmt bytes
            long extra = ch.chunk_size - sizeof(FmtChunk);
            if (extra > 0) fseek(f, extra, SEEK_CUR);
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

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [options] input.wav output.mp3\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -b BITRATE   Bitrate in kbps (default: 128)\n");
    fprintf(stderr, "  -m MODE      mono|stereo|joint (default: auto)\n");
#ifdef GLINT_BOTH_PATHS
    fprintf(stderr, "  -p PATH      double|fixed (default: fixed)\n");
#endif
    fprintf(stderr, "  -s SIMD      auto|avx|sse2|none (default: auto)\n");
}

int main(int argc, char** argv) {
    int bitrate = 128;
    const char* mode_str = nullptr;
    const char* path_str = nullptr;
    const char* simd_str = nullptr;
    const char* input_path = nullptr;
    const char* output_path = nullptr;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-b") == 0 && i + 1 < argc) {
            bitrate = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            mode_str = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            path_str = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            simd_str = argv[++i];
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

    // Open input WAV
    FILE* wav_file = fopen(input_path, "rb");
    if (!wav_file) {
        fprintf(stderr, "Error: cannot open input file '%s'\n", input_path);
        return 1;
    }

    WavInfo wav;
    if (!read_wav_header(wav_file, &wav)) {
        fprintf(stderr, "Error: invalid WAV file\n");
        fclose(wav_file);
        return 1;
    }

    fprintf(stderr, "Input: %d Hz, %d channel(s), 16-bit PCM\n",
            wav.sample_rate, wav.num_channels);

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

    // Validate config
    if (glint_check_config(wav.sample_rate, bitrate) != 0) {
        fprintf(stderr, "Error: invalid sample rate (%d) or bitrate (%d kbps)\n",
                wav.sample_rate, bitrate);
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
        else if (strcmp(simd_str, "none") == 0 || strcmp(simd_str, "scalar") == 0) cfg.simd = GLINT_SIMD_NONE;
        else {
            fprintf(stderr, "Error: invalid SIMD '%s' (use auto|avx|sse2|none)\n", simd_str);
            fclose(wav_file);
            return 1;
        }
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

    fprintf(stderr, "Encoding: %d kbps, %s\n", bitrate,
            mode == GLINT_MONO ? "mono" :
            mode == GLINT_JOINT ? "joint stereo" :
            mode == GLINT_STEREO ? "stereo" : "dual channel");

    // Encode
    int samples_per_frame = glint_samples_per_frame(enc);
    int nch = wav.num_channels;
    int frame_samples = samples_per_frame * nch; // interleaved samples per frame
    int16_t* pcm_buf = new int16_t[frame_samples];
    int16_t* channel_buf[2];
    channel_buf[0] = new int16_t[samples_per_frame];
    channel_buf[1] = new int16_t[samples_per_frame];

    uint32_t total_samples = wav.data_size / (nch * 2); // per channel
    uint32_t samples_encoded = 0;
    uint64_t bytes_written = 0;

    fseek(wav_file, wav.data_offset, SEEK_SET);

    auto start_time = std::chrono::steady_clock::now();

    while (samples_encoded < total_samples) {
        int samples_to_read = samples_per_frame;
        if (samples_encoded + samples_to_read > total_samples) {
            samples_to_read = total_samples - samples_encoded;
        }

        // Read interleaved PCM
        size_t read = fread(pcm_buf, sizeof(int16_t) * nch, samples_to_read, wav_file);
        if (read == 0) break;
        int got = static_cast<int>(read);

        // Deinterleave and zero-pad to full frame
        // If downmixing stereo to mono, average L+R
        bool downmix = (enc_channels == 1 && nch == 2);
        for (int i = 0; i < got; i++) {
            if (downmix) {
                channel_buf[0][i] = (pcm_buf[i * 2] + pcm_buf[i * 2 + 1]) / 2;
            } else {
                channel_buf[0][i] = pcm_buf[i * nch];
                if (nch > 1) {
                    channel_buf[1][i] = pcm_buf[i * nch + 1];
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
    delete[] pcm_buf;
    delete[] channel_buf[0];
    delete[] channel_buf[1];
    glint_destroy(enc);
    fclose(mp3_file);
    fclose(wav_file);

    return 0;
}
