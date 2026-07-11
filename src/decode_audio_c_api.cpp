// glint - whole-file decode C ABI (auto-detects MP3 / AAC-LC / Opus).
// Mirrors the CLI decode pipeline (cli/audio_io.hpp) so every language
// wrapper gets decode_file / transcode_file for free.
// MIT License - Clean-room implementation.

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "aac_decoder.hpp"
#include "glint/glint.h"
#include "mp3_decoder.hpp"
#include "opus_decoder.hpp"
#include "opus_ogg.hpp"

namespace {

enum class Fmt { Unknown, Mp3, Aac, Opus };

Fmt detect(const uint8_t* d, size_t n) {
    if (n >= 4 && !std::memcmp(d, "OggS", 4)) return Fmt::Opus;
    size_t off = 0;
    if (n > 10 && !std::memcmp(d, "ID3", 3))
        off = 10 + ((d[6] & 0x7F) << 21 | (d[7] & 0x7F) << 14 |
                    (d[8] & 0x7F) << 7 | (d[9] & 0x7F));
    if (off + 2 <= n && d[off] == 0xFF && (d[off + 1] & 0xF6) == 0xF0)
        return Fmt::Aac;  // ADTS: 12-bit sync + layer bits 00
    if (off + 2 <= n && d[off] == 0xFF && (d[off + 1] & 0xE0) == 0xE0)
        return Fmt::Mp3;  // MPEG audio syncword 0xFFE (layer bits != 00)
    return Fmt::Unknown;
}

bool decode_mp3(const uint8_t* d, size_t n, std::vector<float>& out,
                int& sr, int& ch) {
    size_t off = 0;
    if (n > 10 && !std::memcmp(d, "ID3", 3))
        off = 10 + ((d[6] & 0x7F) << 21 | (d[7] & 0x7F) << 14 |
                    (d[8] & 0x7F) << 7 | (d[9] & 0x7F));
    glint::mp3::Mp3Decoder dec;
    dec.init();
    float pcm[2 * 1152];
    glint::mp3::Mp3FrameInfo fi;
    sr = ch = 0;
    while (off + 4 <= n) {
        if (glint::mp3::mp3_frame_info(d + off, (int)(n - off), &fi) < 0) {
            off++;
            continue;
        }
        if (fi.frame_bytes <= 0 || off + (size_t)fi.frame_bytes > n) break;
        int s = dec.decode_frame(d + off, (int)(n - off), pcm, &fi);
        if (s > 0) {
            sr = fi.sample_rate;
            ch = fi.channels;
            out.insert(out.end(), pcm, pcm + (size_t)s * fi.channels);
        }
        off += fi.frame_bytes;
    }
    return ch > 0;
}

bool decode_aac(const uint8_t* d, size_t n, std::vector<float>& out,
                int& sr, int& ch) {
    glint::aac::AacDecoder dec;
    dec.init();
    float pcm[2 * 1024];
    glint::aac::AacFrameInfo fi;
    size_t off = 0;
    sr = ch = 0;
    while (off + 7 <= n) {
        if (glint::aac::aac_frame_info(d + off, (int)(n - off), &fi) < 0) {
            off++;
            continue;
        }
        if (fi.frame_bytes <= 0 || off + (size_t)fi.frame_bytes > n) break;
        int s = dec.decode_frame(d + off, (int)(n - off), pcm, &fi);
        if (s > 0) {
            sr = fi.sample_rate;
            ch = fi.channels;
            out.insert(out.end(), pcm, pcm + (size_t)s * fi.channels);
        }
        off += fi.frame_bytes;
    }
    return ch > 0;
}

bool decode_opus(const uint8_t* d, size_t n, std::vector<float>& out,
                 int& sr, int& ch) {
    glint::opus::OggOpusReader r;
    if (r.parse(d, n) != 0) return false;
    ch = r.head().channels;
    if (ch < 1 || ch > 2) return false;
    glint::opus::OpusDecoder dec;
    dec.init(ch);
    std::vector<float> pcm(2 * 5760);
    sr = 48000;
    for (int i = 0; i < r.packet_count(); i++) {
        const auto& p = r.packet(i);
        int s = dec.decode(p.data(), (int)p.size(), pcm.data(), 5760);
        if (s > 0)
            out.insert(out.end(), pcm.data(), pcm.data() + (size_t)s * ch);
    }
    int pre = r.head().pre_skip;
    double gain = r.output_gain();
    if (gain != 1.0)
        for (auto& x : out) x = static_cast<float>(x * gain);
    if (pre > 0 && (long)out.size() >= (long)pre * ch)
        out.erase(out.begin(), out.begin() + (size_t)pre * ch);
    int64_t total = r.total_samples();
    if (total >= 0 && (long)out.size() > total * ch)
        out.resize((size_t)total * ch);
    return true;
}

}  // namespace

extern "C" {

float* glint_decode_audio(const uint8_t* data, int len, int* out_sr,
                          int* out_ch, int* out_frames) {
    if (out_sr) *out_sr = 0;
    if (out_ch) *out_ch = 0;
    if (out_frames) *out_frames = 0;
    if (!data || len <= 0) return nullptr;

    std::vector<float> pcm;
    int sr = 0, ch = 0;
    bool ok = false;
    switch (detect(data, (size_t)len)) {
    case Fmt::Mp3: ok = decode_mp3(data, (size_t)len, pcm, sr, ch); break;
    case Fmt::Aac: ok = decode_aac(data, (size_t)len, pcm, sr, ch); break;
    case Fmt::Opus: ok = decode_opus(data, (size_t)len, pcm, sr, ch); break;
    default: return nullptr;
    }
    if (!ok || ch <= 0) return nullptr;

    int frames = (int)(pcm.size() / (size_t)ch);
    float* buf = static_cast<float*>(std::malloc(sizeof(float) * pcm.size()));
    if (!buf) return nullptr;
    if (!pcm.empty()) std::memcpy(buf, pcm.data(), sizeof(float) * pcm.size());
    if (out_sr) *out_sr = sr;
    if (out_ch) *out_ch = ch;
    if (out_frames) *out_frames = frames;
    return buf;
}

}  // extern "C"
