// glint - Ogg-Vorbis I decoder
// MIT License - Clean-room implementation (from the Vorbis I spec, xiph.org).

#include "vorbis_decoder.hpp"

#include <cmath>
#include <cstring>
#include <functional>

#include "vorbis_bits.hpp"
#include "vorbis_ogg.hpp"

namespace glint {
namespace vorbis {

// ---------------------------------------------------------------------------
// Identification header (spec §4.2.2)
// ---------------------------------------------------------------------------

IdHeader parse_id_header(const uint8_t* pkt, size_t len) {
    IdHeader h;
    // Common header: packet type byte (0x01) + "vorbis".
    if (len < 7 || pkt[0] != 0x01 || std::memcmp(pkt + 1, "vorbis", 6) != 0)
        return h;
    BitReader br(pkt + 7, len - 7);
    uint32_t version = br.read(32);
    int channels = static_cast<int>(br.read(8));
    uint32_t rate = br.read(32);
    br.read(32);  // bitrate_maximum
    br.read(32);  // bitrate_nominal
    br.read(32);  // bitrate_minimum
    int bs = static_cast<int>(br.read(8));
    int bs0 = 1 << (bs & 0x0F);
    int bs1 = 1 << ((bs >> 4) & 0x0F);
    int framing = br.read_bit();
    if (br.overrun()) return h;
    if (version != 0 || channels < 1 || rate < 1 || framing != 1) return h;
    // Blocksizes: powers of two in [64, 8192], short <= long (spec §4.2.2).
    if (bs0 < 64 || bs0 > 8192 || bs1 < 64 || bs1 > 8192 || bs0 > bs1)
        return h;
    h.version = 0;
    h.channels = channels;
    h.sample_rate = rate;
    h.blocksize0 = bs0;
    h.blocksize1 = bs1;
    h.valid = true;
    return h;
}

// ---------------------------------------------------------------------------
// Spec helpers (§9.2)
// ---------------------------------------------------------------------------

float float32_unpack(uint32_t x) {  // spec §9.2.2
    uint32_t mant = x & 0x1fffffu;
    uint32_t sign = x & 0x80000000u;
    uint32_t expo = (x & 0x7fe00000u) >> 21;
    double m = static_cast<double>(mant);
    if (sign) m = -m;
    return static_cast<float>(std::ldexp(m, static_cast<int>(expo) - 788));
}

uint32_t lookup1_values(int entries, int dimensions) {  // spec §9.2.3
    // Greatest integer r such that r^dimensions <= entries.
    if (dimensions <= 0 || entries < 0) return 0;
    uint32_t r = 0;
    for (;;) {
        // Test (r+1)^dimensions <= entries without overflow.
        double p = 1.0;
        uint32_t rr = r + 1;
        for (int d = 0; d < dimensions; d++) p *= rr;
        if (p > static_cast<double>(entries)) break;
        r = rr;
        if (r > 0x10000000u) break;  // safety
    }
    return r;
}

// ---------------------------------------------------------------------------
// Codebooks (spec §3)
// ---------------------------------------------------------------------------

namespace {
constexpr int32_t kNone = -1;
inline int32_t leaf_of(int entry) { return -(entry + 2); }
inline bool is_leaf(int32_t v) { return v <= -2; }
inline int entry_of(int32_t v) { return -v - 2; }
}  // namespace

bool Codebook::build_huffman() {
    child0.assign(1, kNone);
    child1.assign(1, kNone);
    std::vector<char> full(1, 0);  // per node: subtree has no free leaf slot

    int used = 0, single = -1;
    for (int i = 0; i < entries; i++)
        if (lengths[i] > 0) { used++; single = i; }
    single_entry_ = (used == 1) ? single : -1;
    if (single_entry_ >= 0) {
        // Degenerate single-entry codebook: no bits are read on decode.
        for (int i = 0; i < entries; i++)
            if (lengths[i] > 32) return false;
        return true;
    }

    // A slot value can hold more leaves iff it is empty (kNone) or a
    // non-full internal node.
    auto can_take = [&](int32_t slot) -> bool {
        if (slot == kNone) return true;
        if (is_leaf(slot)) return false;
        return !full[slot];
    };
    // Leftmost placement of a leaf at exactly depth L (child0 preferred).
    std::function<bool(int32_t, int, int, int)> place =
        [&](int32_t node, int depth, int L, int entry) -> bool {
        if (depth + 1 == L) {
            if (child0[node] == kNone)
                child0[node] = leaf_of(entry);
            else if (child1[node] == kNone)
                child1[node] = leaf_of(entry);
            else
                return false;
            full[node] = !can_take(child0[node]) && !can_take(child1[node]);
            return true;
        }
        for (int b = 0; b < 2; b++) {
            int32_t slot = b == 0 ? child0[node] : child1[node];
            if (is_leaf(slot)) continue;
            if (slot == kNone) {
                // Allocate a fresh child (may reallocate the arrays).
                child0.push_back(kNone);
                child1.push_back(kNone);
                full.push_back(0);
                slot = static_cast<int32_t>(child0.size() - 1);
                (b == 0 ? child0[node] : child1[node]) = slot;
            }
            if (!full[slot] && place(slot, depth + 1, L, entry)) {
                full[node] =
                    !can_take(child0[node]) && !can_take(child1[node]);
                return true;
            }
        }
        return false;
    };

    for (int i = 0; i < entries; i++) {
        int L = lengths[i];
        if (L == 0) continue;
        if (L > 32) return false;
        if (!place(0, 0, L, i)) return false;  // over-subscribed tree
    }
    return true;
}

void Codebook::build_vq() {
    vq.clear();
    if (lookup_type != 1 && lookup_type != 2) return;
    vq.assign(static_cast<size_t>(entries) * dimensions, 0.f);
    for (int i = 0; i < entries; i++) {
        double last = 0.0;
        if (lookup_type == 1) {
            uint32_t idx_div = 1;
            for (int j = 0; j < dimensions; j++) {
                uint32_t off =
                    lookup_values ? (static_cast<uint32_t>(i) / idx_div) %
                                        static_cast<uint32_t>(lookup_values)
                                  : 0;
                double v = static_cast<double>(multiplicands[off]) *
                               delta_value + minimum_value + last;
                if (sequence_p) last = v;
                vq[static_cast<size_t>(i) * dimensions + j] =
                    static_cast<float>(v);
                idx_div *= static_cast<uint32_t>(lookup_values);
            }
        } else {  // lookup_type == 2
            for (int j = 0; j < dimensions; j++) {
                size_t off = static_cast<size_t>(i) * dimensions + j;
                double v = static_cast<double>(multiplicands[off]) *
                               delta_value + minimum_value + last;
                if (sequence_p) last = v;
                vq[static_cast<size_t>(i) * dimensions + j] =
                    static_cast<float>(v);
            }
        }
    }
}

int Codebook::decode_scalar(BitReader& br) const {
    if (single_entry_ >= 0) return single_entry_;
    int32_t node = 0;
    if (child0.empty()) return -1;
    for (;;) {
        int bit = br.read_bit();
        if (br.overrun()) return -1;
        int32_t next = bit ? child1[node] : child0[node];
        if (next == kNone) return -1;
        if (is_leaf(next)) return entry_of(next);
        node = next;
    }
}

int Codebook::decode_vector(BitReader& br, float* out) const {
    int e = decode_scalar(br);
    if (e < 0 || e >= entries) return -1;
    if (vq.empty()) return -1;
    for (int j = 0; j < dimensions; j++)
        out[j] = vq[static_cast<size_t>(e) * dimensions + j];
    return e;
}

int read_codebook(BitReader& br, Codebook& cb) {
    if (br.read(24) != 0x564342u) return -1;  // "BCV" sync pattern
    cb.dimensions = static_cast<int>(br.read(16));
    cb.entries = static_cast<int>(br.read(24));
    if (cb.entries < 1 || cb.dimensions < 0 || cb.dimensions > 4096)
        return -1;
    if (cb.entries > (1 << 24)) return -1;  // bounded allocation
    cb.lengths.assign(cb.entries, 0);

    int ordered = br.read_bit();
    if (!ordered) {
        int sparse = br.read_bit();
        for (int i = 0; i < cb.entries; i++) {
            if (sparse) {
                if (br.read_bit())
                    cb.lengths[i] = static_cast<uint8_t>(br.read(5) + 1);
                else
                    cb.lengths[i] = 0;
            } else {
                cb.lengths[i] = static_cast<uint8_t>(br.read(5) + 1);
            }
        }
    } else {
        int cur = 0;
        int cur_len = static_cast<int>(br.read(5)) + 1;
        while (cur < cb.entries) {
            int num = static_cast<int>(br.read(ilog(cb.entries - cur)));
            if (cur + num > cb.entries) return -1;
            for (int i = cur; i < cur + num; i++)
                cb.lengths[i] = static_cast<uint8_t>(cur_len);
            cur += num;
            cur_len++;
            if (cur_len > 33) return -1;
        }
    }

    cb.lookup_type = static_cast<int>(br.read(4));
    if (cb.lookup_type == 1 || cb.lookup_type == 2) {
        cb.minimum_value = float32_unpack(br.read(32));
        cb.delta_value = float32_unpack(br.read(32));
        cb.value_bits = static_cast<int>(br.read(4)) + 1;
        cb.sequence_p = br.read_bit();
        if (cb.lookup_type == 1)
            cb.lookup_values =
                static_cast<int>(lookup1_values(cb.entries, cb.dimensions));
        else
            cb.lookup_values = cb.entries * cb.dimensions;
        // Bounded allocation guard: the multiplicands must fit in the packet.
        size_t need_bits =
            static_cast<size_t>(cb.value_bits) * cb.lookup_values;
        if (cb.lookup_values < 0 ||
            need_bits > br.bit_length() - br.bits_read())
            return -1;
        cb.multiplicands.assign(cb.lookup_values, 0);
        for (int i = 0; i < cb.lookup_values; i++)
            cb.multiplicands[i] = br.read(cb.value_bits);
    } else if (cb.lookup_type != 0) {
        return -1;  // reserved
    }

    if (br.overrun()) return -1;
    if (!cb.build_huffman()) return -1;
    cb.build_vq();
    return 0;
}

// ---------------------------------------------------------------------------
// Top-level Ogg-Vorbis decode (audio path lands in a later slice)
// ---------------------------------------------------------------------------

int decode_ogg(const uint8_t* ogg, size_t len, std::vector<float>& pcm,
               int& sample_rate, int& channels) {
    pcm.clear();
    sample_rate = 0;
    channels = 0;
    if (!ogg || len < 27) return -1;

    std::vector<std::vector<uint8_t>> packets;
    int64_t last_granule = -1;
    if (ogg_demux_first_stream(ogg, len, packets, &last_granule) != 0)
        return -1;
    if (packets.size() < 3) return -1;  // need the 3 header packets

    // Packet 0 must be the Vorbis identification header.
    IdHeader id = parse_id_header(packets[0].data(), packets[0].size());
    if (!id.valid) return -1;
    // Packet 1 = comment header (contents skipped), packet 2 = setup header.
    if (packets[1].size() < 7 || packets[1][0] != 0x03 ||
        std::memcmp(packets[1].data() + 1, "vorbis", 6) != 0)
        return -1;
    if (packets[2].size() < 7 || packets[2][0] != 0x05 ||
        std::memcmp(packets[2].data() + 1, "vorbis", 6) != 0)
        return -1;

    sample_rate = static_cast<int>(id.sample_rate);
    channels = id.channels;

    // Audio decode (setup parse + per-packet synthesis) is implemented in a
    // subsequent slice; headers validate here.
    return -1;
}

}  // namespace vorbis
}  // namespace glint
