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
#include "wav_io.hpp"

namespace glint_cli {

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

// ---- WAV / raw parse + write: delegate to the shared src/wav_io.* ----
inline bool parse_wav(const uint8_t* d, size_t n, Audio& a,
                      std::string& err) {
    if (!glint::wav_read(d, n, a.pcm, a.sr, a.ch)) {
        err = "not a RIFF/WAVE file or unsupported sample format";
        return false;
    }
    return true;
}

inline bool parse_raw(const uint8_t* d, size_t n, int sr, int ch, int bits,
                      Audio& a) {
    a.sr = sr;
    a.ch = ch;
    return glint::pcm_read(d, n, sr, ch, bits, a.pcm);
}

// 16-bit PCM by default; pass bits/is_float to widen (8/24/32-int, float).
inline std::vector<uint8_t> write_wav(const Audio& a, int bits,
                                      bool is_float) {
    return glint::wav_write(a.pcm.data(), a.frames(), a.ch, a.sr, bits,
                            is_float);
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
