// glint - Ogg-Vorbis I decoder
// MIT License - Clean-room implementation (from the Vorbis I spec, xiph.org).

#include "vorbis_decoder.hpp"

#include <cmath>
#include <cstring>

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
