#!/usr/bin/env python3
"""Hand-build minimal but VALID MPEG-1 Layer III frames to exercise the
decoder's intensity-stereo path — no MP3 encoder in the wild emits
intensity stereo anymore (LAME/ffmpeg both disable it), so this is the
only way to get ffmpeg-as-oracle coverage of that code path.

Emits a short stream of identical frames. Left channel carries a fixed
±1 spectral pattern across the band range; the right channel is empty
above the intensity bound, and its scalefactors are the per-band
intensity positions. Both glint and ffmpeg must decode it identically.

usage: gen_mp3_intensity.py <out.mp3> [is_bound_band] [--nostereo-is]
  is_bound_band: band index where the intensity region starts (default 0
                 = whole spectrum is intensity). Right channel is coded
                 with big_values reaching that band.
"""

import struct
import sys

SFB_L = [0, 4, 8, 12, 16, 20, 24, 30, 36, 44, 52, 62, 74, 90, 110, 134,
         162, 196, 238, 288, 342, 418, 576]

# Huffman table 1 (2x2): len[x*2+y], code (right-aligned).
HT1_LEN = [1, 3, 2, 3]
HT1_CODE = [1, 1, 1, 0]

# scalefac_compress -> (slen1, slen2)
SLEN1 = [0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4]
SLEN2 = [0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3]


class BitWriter:
    def __init__(self):
        self.acc = 0
        self.nbits = 0
        self.out = bytearray()

    def put(self, val, n):
        for i in range(n - 1, -1, -1):
            self.acc = (self.acc << 1) | ((val >> i) & 1)
            self.nbits += 1
            if self.nbits == 8:
                self.out.append(self.acc & 0xFF)
                self.acc = 0
                self.nbits = 0

    def align(self):
        if self.nbits:
            self.acc <<= (8 - self.nbits)
            self.out.append(self.acc & 0xFF)
            self.acc = 0
            self.nbits = 0

    def bitpos(self):
        return len(self.out) * 8 + self.nbits


def pair_bits(x, y):
    """(code, nbits) for table-1 pair plus sign bits (append separately)."""
    idx = (1 if x else 0) * 2 + (1 if y else 0)
    return HT1_CODE[idx], HT1_LEN[idx]


def build_granule_channel(bw, spectrum, big_values, scalefactors,
                          slen1, slen2):
    """Write scalefactors + Huffman big_values; return the bit length
    (== part2_3_length; the count1 region is empty)."""
    start = bw.bitpos()
    for b in range(21):
        slen = slen1 if b < 11 else slen2
        if slen:
            bw.put(scalefactors[b] & ((1 << slen) - 1), slen)
    for p in range(big_values):
        x = spectrum[2 * p]
        y = spectrum[2 * p + 1]
        code, n = pair_bits(abs(x), abs(y))
        bw.put(code, n)
        if x:
            bw.put(1 if x < 0 else 0, 1)
        if y:
            bw.put(1 if y < 0 else 0, 1)
    return bw.bitpos() - start


def main():
    out_path = sys.argv[1]
    args = sys.argv[2:]
    is_bound_band = 0
    mode_ext = 1        # 01 = intensity only; 3 = MS + intensity
    pos7 = False        # sprinkle is_pos = 7 ("no intensity") bands
    i = 0
    while i < len(args):
        if args[i] == "--ms-is":
            mode_ext = 3
        elif args[i] == "--pos7":
            pos7 = True
        else:
            is_bound_band = int(args[i])
        i += 1

    # Left spectrum: ±1 across bands 0..20 (skip top sfb21 tail for
    # simplicity; keep within big_values=288 -> 576 lines).
    left = [0] * 576
    for i in range(SFB_L[is_bound_band], SFB_L[20]):
        left[i] = 1 if (i // 4) % 2 == 0 else -1
    # Also give the intensity region (>= is_bound) a strong pattern so
    # the position math is audible.
    for i in range(SFB_L[is_bound_band], 288 * 2):
        left[i] = 1 if (i % 3) else -1

    right = [0] * 576
    # Right carries real low-band content below the intensity bound.
    for i in range(0, SFB_L[is_bound_band]):
        right[i] = 1 if (i // 4) % 2 else -1

    left_bv = 288           # covers all 576 lines
    right_bv = SFB_L[is_bound_band] // 2  # ends exactly at the bound band

    # Right scalefactors above the bound = intensity positions (cycle
    # 0..6; 7 would mean "no intensity"). Below the bound they are real
    # (keep 0). Use scalefac_compress=13 -> slen 3/3 (positions 0..7).
    sfc_right = 13
    s1r, s2r = SLEN1[sfc_right], SLEN2[sfc_right]
    right_scf = [0] * 21
    for b in range(is_bound_band, 21):
        right_scf[b] = 7 if (pos7 and b % 4 == 0) else (b % 6)

    # Left uses compress 0 (all scalefactors zero, no scf bits).
    sfc_left = 0
    s1l, s2l = SLEN1[sfc_left], SLEN2[sfc_left]
    left_scf = [0] * 21

    global_gain = 200  # ~0.18 amplitude for ±1 lines; avoids clipping

    # ---- build main data for 2 granules x 2 channels; record the
    # actual part2_3 length of each ----
    md = BitWriter()
    p23 = []  # [gr][ch]
    for gr in range(2):
        n0 = build_granule_channel(md, left, left_bv, left_scf, s1l, s2l)
        n1 = build_granule_channel(md, right, right_bv, right_scf, s1r,
                                   s2r)
        p23.append((n0, n1))
        if max(n0, n1) >= 4096:
            raise SystemExit("part2_3 exceeds 12-bit field")
    md.align()
    main_data = bytes(md.out)

    # ---- side info (stereo MPEG-1 = 32 bytes) ----
    si = BitWriter()
    si.put(0, 9)     # main_data_begin = 0 (self-contained)
    si.put(0, 3)     # private_bits
    si.put(0, 4)     # scfsi ch0
    si.put(0, 4)     # scfsi ch1
    for gr in range(2):
        for ch in range(2):
            bv = left_bv if ch == 0 else right_bv
            sfc = sfc_left if ch == 0 else sfc_right
            si.put(p23[gr][ch], 12)  # part2_3_length (actual)
            si.put(bv, 9)            # big_values
            si.put(global_gain, 8)   # global_gain
            si.put(sfc, 4)           # scalefac_compress
            si.put(0, 1)             # window_switching = 0 (long)
            si.put(1, 5)             # table_select[0] = 1
            si.put(1, 5)             # table_select[1] = 1
            si.put(1, 5)             # table_select[2] = 1
            # region0_count / region1_count: cover the whole big_values
            # with region0 -> set region boundaries generously.
            si.put(15, 4)            # region0_count (sfb_l[16] = 162)
            si.put(6, 3)             # region1_count -> reaches sfb_l[23]
            si.put(0, 1)             # preflag
            si.put(0, 1)             # scalefac_scale
            si.put(0, 1)             # count1table_select
    si.align()
    side = bytes(si.out)
    assert len(side) == 32, len(side)

    # ---- header ----
    # 128 kbps, 44100, no padding, joint stereo, mode_ext = intensity.
    # We must size the frame to hold header+side+main_data.
    frame_bytes = 4 + 32 + len(main_data)
    # Standard 128k/44.1 frame is 417/418 bytes; our main data may be
    # larger, so pick the smallest standard bitrate whose frame fits.
    BR = [32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
    chosen = None
    for i, kb in enumerate(BR):
        fb = 144000 * kb // 44100
        if fb >= frame_bytes:
            chosen = (i + 1, kb, fb)
            break
    if not chosen:
        raise SystemExit("main data too large for any bitrate")
    bidx, kbps, fb = chosen

    hdr = bytearray(4)
    hdr[0] = 0xFF
    hdr[1] = 0xFB  # MPEG1, Layer III, no CRC
    hdr[2] = (bidx << 4) | (0 << 2) | (0 << 1)  # bitrate, sr=44100, pad0
    hdr[3] = (1 << 6) | (mode_ext << 4)  # mode=joint(01), mode_ext

    frame = bytes(hdr) + side + main_data
    frame += b"\x00" * (fb - len(frame))  # pad to full frame size

    with open(out_path, "wb") as f:
        # A few identical frames so the decoder settles.
        for _ in range(8):
            f.write(frame)

    print(f"wrote {out_path}: bitrate={kbps}k frame={fb}B "
          f"mode_ext={mode_ext} pos7={pos7} "
          f"is_bound_band={is_bound_band} main_data={len(main_data)}B")


if __name__ == "__main__":
    main()
