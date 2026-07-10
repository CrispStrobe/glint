// Opus packet decoding — RFC 6716 section 3
// MIT License - Clean-room implementation

#include "opus_decoder.hpp"

#include <cmath>
#include <cstring>

#include "opus_mdct.hpp"

namespace glint {
namespace opus {

void pcm_soft_clip(float* x_, int n, int channels, float* declip_mem) {
    if (channels < 1 || n < 1 || !x_ || !declip_mem) return;

    // The nonlinearity's derivative hits zero at ±2; saturate there first.
    for (int i = 0; i < n * channels; i++) {
        float v = x_[i];
        x_[i] = v < -2.f ? -2.f : (v > 2.f ? 2.f : v);
    }
    for (int c = 0; c < channels; c++) {
        float* x = x_ + c;
        float a = declip_mem[c];
        // Keep applying the previous frame's curve until a sign change so
        // the nonlinearity is continuous across the frame boundary.
        for (int i = 0; i < n; i++) {
            if (x[i * channels] * a >= 0) break;
            x[i * channels] += a * x[i * channels] * x[i * channels];
        }

        int curr = 0;
        float x0 = x[0];
        for (;;) {
            int i;
            for (i = curr; i < n; i++) {
                if (x[i * channels] > 1 || x[i * channels] < -1) break;
            }
            if (i == n) {
                a = 0;
                break;
            }
            int peak_pos = i;
            int start = i, end = i;
            float maxval = std::fabs(x[i * channels]);
            // Expand to the zero crossings around the excursion, tracking
            // the biggest peak inside it.
            while (start > 0 &&
                   x[i * channels] * x[(start - 1) * channels] >= 0)
                start--;
            while (end < n && x[i * channels] * x[end * channels] >= 0) {
                if (std::fabs(x[end * channels]) > maxval) {
                    maxval = std::fabs(x[end * channels]);
                    peak_pos = end;
                }
                end++;
            }
            int special = start == 0 && x[i * channels] * x[0] >= 0;

            // a such that maxval + a*maxval^2 == 1 (tiny boost keeps
            // -ffast-math builds strictly inside ±1).
            a = (maxval - 1) / (maxval * maxval);
            a += a * 2.4e-7f;
            if (x[i * channels] > 0) a = -a;
            for (i = start; i < end; i++)
                x[i * channels] += a * x[i * channels] * x[i * channels];

            if (special && peak_pos >= 2) {
                // Ramp from the frame's first sample to the peak to avoid a
                // discontinuity at the frame boundary.
                float offset = x0 - x[0];
                float delta = offset / peak_pos;
                for (i = curr; i < peak_pos; i++) {
                    offset -= delta;
                    x[i * channels] += offset;
                    float v = x[i * channels];
                    x[i * channels] = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
                }
            }
            curr = end;
            if (curr == n) break;
        }
        declip_mem[c] = a;
    }
}

namespace {

// One- or two-byte frame length (RFC 3.2.1). Returns bytes consumed, -1 on
// truncation; *size gets the frame length (0..1275).
int parse_size(const uint8_t* data, int32_t len, int16_t* size) {
    if (len < 1) {
        *size = -1;
        return -1;
    }
    if (data[0] < 252) {
        *size = data[0];
        return 1;
    }
    if (len < 2) {
        *size = -1;
        return -1;
    }
    *size = static_cast<int16_t>(4 * data[1] + data[0]);
    return 2;
}

}  // namespace

int opus_packet_parse(const uint8_t* data, int32_t len, OpusPacket* pkt) {
    if (len < 1) return -1;
    const uint8_t toc = data[0];
    pkt->config = toc >> 3;
    pkt->stereo = (toc >> 2) & 1;
    data++;
    len--;

    // Audio size from config: CELT-only (16..31) uses 2.5/5/10/20 ms.
    if (pkt->config >= 16) {
        pkt->frame_size = 120 << (pkt->config & 3);
    } else if (pkt->config < 12) {
        // SILK-only: 10/20/40/60 ms.
        static const int kSilkSizes[4] = { 480, 960, 1920, 2880 };
        pkt->frame_size = kSilkSizes[pkt->config & 3];
    } else {
        // Hybrid: 10/20 ms.
        pkt->frame_size = 480 << (pkt->config & 1);
    }

    int count;
    int16_t size0;
    const int code = toc & 3;
    int32_t pad = 0;
    bool cbr = true;
    switch (code) {
    case 0:
        count = 1;
        pkt->sizes[0] = static_cast<int16_t>(len);
        break;
    case 1:
        if (len & 1) return -4;
        count = 2;
        pkt->sizes[0] = pkt->sizes[1] = static_cast<int16_t>(len / 2);
        break;
    case 2: {
        count = 2;
        int used = parse_size(data, len, &size0);
        if (used < 0 || size0 > len - used) return -4;
        pkt->sizes[0] = size0;
        pkt->sizes[1] = static_cast<int16_t>(len - used - size0);
        data += used;
        len -= used;
        cbr = false;
        break;
    }
    default: {  // code 3: arbitrary count, optional padding, CBR or VBR
        if (len < 1) return -4;
        const uint8_t ch = data[0];
        count = ch & 0x3F;
        if (count <= 0 || count > 48 ||
            pkt->frame_size * count > 5760 /* 120 ms cap */)
            return -4;
        const bool vbr = (ch & 0x80) != 0;
        const bool padded = (ch & 0x40) != 0;
        data++;
        len--;
        if (padded) {
            // Padding length: 255 bytes chain as 254 + continue.
            for (;;) {
                if (len <= 0) return -4;
                int p = data[0];
                data++;
                len--;
                pad += p == 255 ? 254 : p;
                if (p != 255) break;
            }
        }
        if (pad > len) return -4;
        len -= pad;
        if (vbr) {
            cbr = false;
            int32_t total = 0;
            for (int i = 0; i < count - 1; i++) {
                int used = parse_size(data, len, &pkt->sizes[i]);
                if (used < 0) return -4;
                data += used;
                len -= used;
                if (pkt->sizes[i] > len - total) return -4;
                total += pkt->sizes[i];
            }
            pkt->sizes[count - 1] = static_cast<int16_t>(len - total);
        }
        if (cbr) {
            if (len % count) return -4;
            for (int i = 0; i < count; i++)
                pkt->sizes[i] = static_cast<int16_t>(len / count);
        }
        break;
    }
    }

    // A frame must not exceed the 1275-byte cap.
    int32_t offset = 0;
    for (int i = 0; i < count; i++) {
        if (pkt->sizes[i] < 0 || pkt->sizes[i] > 1275) return -4;
        pkt->frames[i] = data + offset;
        offset += pkt->sizes[i];
    }
    if (offset > len) return -4;
    pkt->frame_count = count;
    return 0;
}

namespace {
// The CELT overlap window doubles as the transition fade (2.5 ms @48k).
const double* celt_window() {
    static double w[120];
    static bool once = [] {
        mdct_window_fill(w, 120);
        return true;
    }();
    (void)once;
    return w;
}

// Crossfade in1 -> in2 with the squared CELT overlap window (2.5 ms).
void smooth_fade(const float* in1, const float* in2, float* out,
                 int overlap, int channels, const double* window) {
    for (int c = 0; c < channels; c++) {
        for (int i = 0; i < overlap; i++) {
            float w = static_cast<float>(window[i] * window[i]);
            out[i * channels + c] = w * in2[i * channels + c] +
                                    (1.0f - w) * in1[i * channels + c];
        }
    }
}
}  // namespace

int OpusDecoder::decode_frame_impl(const uint8_t* data, int16_t size,
                                   float* pcm, const OpusPacket& pkt) {
    const int stream_channels = pkt.stereo ? 2 : 1;
    // Mode from the TOC config: SILK-only 0..11, hybrid 12..15,
    // CELT-only 16..31.
    const int mode = pkt.config < 12 ? 1 : (pkt.config < 16 ? 2 : 3);
    // CELT end band by bandwidth (MB has no CELT layer; it maps to 17).
    int end_band = 21;
    if (mode == 1)
        end_band = (pkt.config >> 2) == 0 ? 13 : 17;  // NB / MB+WB
    else if (mode == 2)
        end_band = (pkt.config & 2) ? 21 : 19;  // SWB 19, FB 21
    else
        end_band = ((pkt.config >> 2) & 3) == 0
                       ? 13
                       : (((pkt.config >> 2) & 3) == 1
                              ? 17
                              : (((pkt.config >> 2) & 3) == 2 ? 19 : 21));

    RangeDecoder dec;
    dec.init(data, static_cast<uint32_t>(size));

    // A mode switch invalidates CELT's synthesis history — unless the
    // previous packet carried SILK->CELT redundancy, which already reset
    // and primed it.
    if (mode != prev_mode_ && prev_mode_ > 0 && !prev_redundancy_)
        celt_.init(channels_);

    int32_t len = size;
    int16_t pcm_silk[2 * 2880];
    int n_silk = 0;
    if (mode != 3) {
        // SILK part: internal rate from bandwidth (hybrid is always WB);
        // 40/60 ms packets decode as chained 20 ms SILK frames.
        int internal_khz;
        if (mode == 2)
            internal_khz = 16;
        else
            internal_khz = 8 + 4 * ((pkt.config >> 2) & 3);  // NB/MB/WB
        int payload_ms = pkt.frame_size / 48;
        while (n_silk < pkt.frame_size) {
            int ret = silk_.decode(dec, pcm_silk + n_silk * channels_,
                                   channels_, stream_channels,
                                   internal_khz, 48000, payload_ms,
                                   n_silk == 0);
            if (ret < 0) return ret;
            n_silk += ret;
        }
    }

    // Transition redundancy: a 5 ms CELT frame appended to the payload.
    // Hybrid signals it with a flag; SILK-only implies it whenever enough
    // bits are left over.
    int redundancy = 0, celt_to_silk = 0;
    int32_t redundancy_bytes = 0;
    if (mode != 3 &&
        static_cast<int32_t>(dec.tell()) + 17 + 20 * (mode == 2) <=
            8 * len) {
        redundancy = mode == 2 ? dec.dec_bit_logp(12) : 1;
        if (redundancy) {
            celt_to_silk = dec.dec_bit_logp(1);
            redundancy_bytes =
                mode == 2
                    ? static_cast<int32_t>(dec.dec_uint(256)) + 2
                    : len - ((static_cast<int32_t>(dec.tell()) + 7) >> 3);
            len -= redundancy_bytes;
            if (len * 8 < static_cast<int32_t>(dec.tell())) {
                len = 0;
                redundancy_bytes = 0;
                redundancy = 0;
            }
            // The redundant frame's bytes leave the main frame's budget
            // (raw bits at the buffer end belong to the redundancy).
            dec.shrink(static_cast<uint32_t>(redundancy_bytes));
        }
    }

    float redundant_audio[2 * 240];
    uint32_t redundant_rng = 0;
    if (redundancy && celt_to_silk) {
        // CELT->SILK: the redundant frame continues the old CELT state.
        RangeDecoder rdec;
        rdec.init(data + len, static_cast<uint32_t>(redundancy_bytes));
        int ret = celt_.decode_frame(
            rdec, static_cast<uint32_t>(redundancy_bytes), redundant_audio,
            240, stream_channels, end_band, 0);
        if (ret < 0) return ret;
        redundant_rng = rdec.range();
    }

    if (mode != 1) {
        // CELT part (hybrid: same range decoder, bands from 17).
        int start_band = mode == 2 ? 17 : 0;
        int celt_frame = pkt.frame_size < 960 ? pkt.frame_size : 960;
        int ret = celt_.decode_frame(dec, static_cast<uint32_t>(len), pcm,
                                     celt_frame, stream_channels, end_band,
                                     start_band);
        if (ret < 0) return ret;
    } else {
        std::memset(pcm, 0,
                    static_cast<size_t>(pkt.frame_size) * channels_ *
                        sizeof(float));
    }

    if (mode != 3) {
        // Mix in the SILK output (CELT wrote pcm already in hybrid).
        for (int i = 0; i < n_silk * channels_; i++)
            pcm[i] += pcm_silk[i] * (1.0f / 32768.0f);
    }

    if (redundancy && !celt_to_silk) {
        // SILK->CELT: prime a fresh CELT state with the redundant frame,
        // then fade this frame's tail into it.
        celt_.init(channels_);
        RangeDecoder rdec;
        rdec.init(data + len, static_cast<uint32_t>(redundancy_bytes));
        int ret = celt_.decode_frame(
            rdec, static_cast<uint32_t>(redundancy_bytes), redundant_audio,
            240, stream_channels, end_band, 0);
        if (ret < 0) return ret;
        redundant_rng = rdec.range();
        smooth_fade(pcm + channels_ * (pkt.frame_size - 120),
                    redundant_audio + channels_ * 120,
                    pcm + channels_ * (pkt.frame_size - 120), 120,
                    channels_, celt_window());
    }
    if (redundancy && celt_to_silk) {
        // CELT->SILK: the redundant frame owns the first 2.5 ms, then
        // fades into the SILK output.
        for (int c = 0; c < channels_; c++)
            for (int i = 0; i < 120; i++)
                pcm[channels_ * i + c] = redundant_audio[channels_ * i + c];
        smooth_fade(redundant_audio + channels_ * 120,
                    pcm + channels_ * 120, pcm + channels_ * 120, 120,
                    channels_, celt_window());
    }

    prev_mode_ = mode;
    prev_redundancy_ = redundancy && !celt_to_silk;
    final_range_ = dec.range() ^ redundant_rng;
    return pkt.frame_size;
}

int OpusDecoder::decode(const uint8_t* data, int32_t len, float* pcm,
                        int max_samples) {
    OpusPacket pkt;
    int err = opus_packet_parse(data, len, &pkt);
    if (err) return err;
    if (pkt.frame_count * pkt.frame_size > max_samples) return -2;

    int total = 0;
    for (int f = 0; f < pkt.frame_count; f++) {
        if (pkt.sizes[f] < 1) return -4;  // PLC/DTX frames: later
        int ret = decode_frame_impl(pkt.frames[f], pkt.sizes[f],
                                    pcm + total * channels_, pkt);
        if (ret < 0) return ret;
        total += ret;
    }
    return total;
}

}  // namespace opus
}  // namespace glint
