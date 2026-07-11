// glint - command-line codec Swiss-army-knife.
// Encode, decode and transcode MP3 / AAC-LC / Opus, plus WAV/raw I/O,
// resampling, gain and normalize, over a universal float PCM pipeline.
// MIT License - Clean-room implementation.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "audio_io.hpp"

using namespace glint_cli;

static void usage(const char* prog) {
    std::fprintf(stderr,
        "glint - MP3/AAC/Opus encode, decode, transcode\n"
        "Usage: %s [options] <input> <output>\n"
        "       %s --info <input>\n"
        "  <input>/<output>: file path, or '-' for stdin/stdout.\n"
        "  Format is chosen by extension (.wav .mp3 .aac .opus .raw) or -F.\n\n"
        "Options:\n"
        "  -F FMT       output format: wav|mp3|aac|opus|raw (override extension)\n"
        "  -b KBPS      bitrate in kbps (default 128)\n"
        "  -V Q         VBR quality 0-9 (0=best/largest)\n"
        "  -m MODE      mono|stereo|joint (encode channel mode)\n"
        "  -q QUAL      speed|normal|best (MP3/AAC encode effort)\n"
        "  -r R:C:B     treat raw input as PCM (e.g. 44100:2:16)\n"
        "  -p PATH      double|fixed signal path (build-dependent)\n"
        "  -j N         encoder worker threads\n"
        "  --rate HZ    resample to HZ before encoding\n"
        "  --bits N     WAV/raw output bit depth: 8|16|24|32 (default 16)\n"
        "  --wav-float  write float WAV/raw (--bits 32 or 64)\n"
        "  --gain DB    apply gain in dB\n"
        "  --norm[=DB]  peak-normalize (default -1 dBFS)\n"
        "  --info       print input file info and exit\n",
        prog, prog);
}

static const char* fmt_str(Fmt f) {
    switch (f) {
    case Fmt::Wav: return "WAV";
    case Fmt::Raw: return "raw PCM";
    case Fmt::Mp3: return "MP3";
    case Fmt::Aac: return "AAC-LC";
    case Fmt::Opus: return "Opus";
    default: return "unknown";
    }
}

int main(int argc, char** argv) {
    int bitrate = 128, vbr_q = -1, threads = 1, out_rate = 0;
    int out_bits = 16;
    bool wav_float = false;
    const char* out_fmt_flag = nullptr;
    const char* mode_str = nullptr;
    const char* quality_str = nullptr;
    const char* raw_spec = nullptr;
    const char* path_str = nullptr;
    bool do_info = false, do_norm = false;
    double gain_db = 0.0, norm_db = -1.0;
    std::string input, output;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* n) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Error: %s needs an argument\n", n);
                std::exit(1);
            }
            return argv[++i];
        };
        if (a == "-b") bitrate = std::atoi(next("-b"));
        else if (a == "-F") out_fmt_flag = next("-F");
        else if (a == "-V") vbr_q = std::atoi(next("-V"));
        else if (a == "-m") mode_str = next("-m");
        else if (a == "-q") quality_str = next("-q");
        else if (a == "-r") raw_spec = next("-r");
        else if (a == "-p") path_str = next("-p");
        else if (a == "-s") next("-s");  // accepted, no-op in this build
        else if (a == "-j") threads = std::atoi(next("-j"));
        else if (a == "--rate") out_rate = std::atoi(next("--rate"));
        else if (a == "--bits") out_bits = std::atoi(next("--bits"));
        else if (a == "--wav-float") wav_float = true;
        else if (a == "--gain") gain_db = std::atof(next("--gain"));
        else if (a == "--info") do_info = true;
        else if (a == "--norm") { do_norm = true; }
        else if (a.rfind("--norm=", 0) == 0) {
            do_norm = true;
            norm_db = std::atof(a.c_str() + 7);
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        } else if (input.empty()) input = a;
        else if (output.empty()) output = a;
        else { usage(argv[0]); return 1; }
    }
    (void)path_str;
    if (input.empty() || (output.empty() && !do_info)) {
        usage(argv[0]);
        return 1;
    }
    if (threads > 1) glint_set_threads(threads);

    // ---- load input ----
    std::vector<uint8_t> raw = read_all(input);
    if (raw.empty()) {
        std::fprintf(stderr, "Error: cannot read '%s'\n", input.c_str());
        return 1;
    }
    Fmt ifmt;
    int rr = 0, rc = 0, rb = 0;
    if (raw_spec) {
        if (std::sscanf(raw_spec, "%d:%d:%d", &rr, &rc, &rb) != 3) {
            std::fprintf(stderr, "Error: bad -r spec '%s'\n", raw_spec);
            return 1;
        }
        ifmt = Fmt::Raw;
    } else {
        ifmt = fmt_from_ext(input);
        if (ifmt == Fmt::Unknown || ifmt == Fmt::Wav) {
            Fmt m = fmt_from_magic(raw.data(), raw.size());
            if (m != Fmt::Unknown) ifmt = m;
        }
    }

    Audio audio;
    std::string err;
    bool ok = false;
    switch (ifmt) {
    case Fmt::Wav: ok = parse_wav(raw.data(), raw.size(), audio, err); break;
    case Fmt::Raw: ok = parse_raw(raw.data(), raw.size(), rr, rc, rb, audio); break;
    case Fmt::Mp3: ok = decode_mp3(raw.data(), raw.size(), audio); break;
    case Fmt::Aac: ok = decode_aac(raw.data(), raw.size(), audio); break;
    case Fmt::Opus: ok = decode_opus(raw.data(), raw.size(), audio); break;
    default: std::fprintf(stderr, "Error: unknown input format\n"); return 1;
    }
    if (!ok || audio.ch == 0) {
        std::fprintf(stderr, "Error: failed to read %s input%s%s\n",
                     fmt_str(ifmt), err.empty() ? "" : ": ", err.c_str());
        return 1;
    }

    // ---- --info ----
    if (do_info) {
        double dur = audio.sr ? (double)audio.frames() / audio.sr : 0;
        std::fprintf(stderr, "%s: %s, %d Hz, %d ch, %.2f s (%ld frames)\n",
                     input.c_str(), fmt_str(ifmt), audio.sr, audio.ch, dur,
                     audio.frames());
        return 0;
    }

    // ---- output format ----
    Fmt ofmt = Fmt::Unknown;
    if (out_fmt_flag) {
        std::string f = out_fmt_flag;
        if (f == "wav") ofmt = Fmt::Wav;
        else if (f == "mp3") ofmt = Fmt::Mp3;
        else if (f == "aac") ofmt = Fmt::Aac;
        else if (f == "opus") ofmt = Fmt::Opus;
        else if (f == "raw") ofmt = Fmt::Raw;
    } else {
        ofmt = fmt_from_ext(output);
    }
    if (ofmt == Fmt::Unknown) {
        std::fprintf(stderr, "Error: unknown output format (use -F)\n");
        return 1;
    }

    // ---- process: rate, gain, normalize ----
    int target_rate = out_rate;
    if (ofmt == Fmt::Opus && target_rate == 0 && audio.sr != 48000)
        target_rate = 48000;  // Opus is 48 kHz only
    if (target_rate && target_rate != audio.sr) {
        Audio r2 = to_rate(audio, target_rate);
        std::fprintf(stderr, "Resampled %d -> %d Hz\n", audio.sr,
                     target_rate);
        audio = std::move(r2);
    }
    if (gain_db != 0.0) apply_gain(audio, gain_db);
    if (do_norm) normalize(audio, norm_db);

    int mode = -1;
    bool mono = false;
    if (mode_str) {
        if (!std::strcmp(mode_str, "mono")) { mode = GLINT_MONO; mono = true; }
        else if (!std::strcmp(mode_str, "stereo")) mode = GLINT_STEREO;
        else if (!std::strcmp(mode_str, "joint")) mode = GLINT_JOINT;
    }
    int quality = GLINT_QUALITY_SPEED;
    if (quality_str) {
        if (!std::strcmp(quality_str, "normal")) quality = GLINT_QUALITY_NORMAL;
        else if (!std::strcmp(quality_str, "best")) quality = GLINT_QUALITY_BEST;
    }

    // ---- store output ----
    std::vector<uint8_t> outbytes;
    switch (ofmt) {
    case Fmt::Wav:
        outbytes = write_wav(audio, out_bits, wav_float);
        break;
    case Fmt::Raw:
        outbytes = write_wav(audio, out_bits, wav_float);
        outbytes.erase(outbytes.begin(), outbytes.begin() + 44);
        break;
    case Fmt::Mp3:
        outbytes = encode_mp3(audio, bitrate, vbr_q, mode, quality);
        break;
    case Fmt::Aac:
        outbytes = encode_aac(audio, bitrate, vbr_q, mono, quality);
        break;
    case Fmt::Opus:
        outbytes = encode_opus(audio, bitrate * 1000, vbr_q >= 0);
        break;
    default: break;
    }
    if (outbytes.empty()) {
        std::fprintf(stderr, "Error: encoding to %s produced no output "
                     "(unsupported config?)\n", fmt_str(ofmt));
        return 1;
    }
    if (!write_all(output, outbytes.data(), outbytes.size())) {
        std::fprintf(stderr, "Error: cannot write '%s'\n", output.c_str());
        return 1;
    }
    std::fprintf(stderr, "%s -> %s: %d Hz %d ch, %zu bytes\n",
                 fmt_str(ifmt), fmt_str(ofmt), audio.sr, audio.ch,
                 outbytes.size());
    return 0;
}
