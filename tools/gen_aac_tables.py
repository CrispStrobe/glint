#!/usr/bin/env python3
"""Generate src/aac_tables.hpp — ISO/IEC 13818-7 / 14496-3 standard AAC tables.

The scalefactor-band offsets and Huffman codebooks are normative data from the
ISO spec (Annex A / Table 4.5.x). To rule out transcription errors, this
script extracts the same tables from TWO independent implementations —
vo-aacenc (Apache-2.0) and ffmpeg's libavcodec/aactab.c — cross-checks them
bit-for-bit, verifies each codebook is prefix-free, and only then emits the
C++ header. Run from the repo root:

    python tools/gen_aac_tables.py [--srcdir DIR]

If --srcdir is not given, the two source files are downloaded into a temp dir.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

VO_URL = "https://raw.githubusercontent.com/mstorsjo/vo-aacenc/master/aacenc/src/aac_rom.c"
FF_URL = "https://raw.githubusercontent.com/FFmpeg/FFmpeg/master/libavcodec/aactab.c"


def fetch(srcdir):
    paths = {}
    for name, url in (("aac_rom.c", VO_URL), ("aactab.c", FF_URL)):
        p = os.path.join(srcdir, name)
        if not os.path.exists(p):
            subprocess.check_call(["curl", "-sL", "-o", p, url])
        paths[name] = p
    return paths


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def extract_array(text, name):
    """Return the flat list of integers in the initializer of `name`."""
    m = re.search(re.escape(name) + r"\s*(?:\[[^\]]*\]\s*)*=\s*\{", text)
    if not m:
        raise KeyError(name)
    depth, i = 1, m.end()
    start = m.end()
    while depth > 0:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    body = text[start : i - 1]
    return [int(tok, 0) for tok in re.findall(r"0[xX][0-9a-fA-F]+|\d+", body)]


def check_prefix_free(codes, lens, label):
    seen = {}
    for c, l in zip(codes, lens):
        assert 0 < l <= 32 and c < (1 << l), f"{label}: code 0x{c:x} overflows len {l}"
        seen[(c, l)] = seen.get((c, l), 0) + 1
    items = list(zip(codes, lens))
    for i, (c1, l1) in enumerate(items):
        for c2, l2 in items[i + 1 :]:
            if l1 == l2 and c1 == c2:
                raise AssertionError(f"{label}: duplicate codeword")
            lo, hi = ((c1, l1), (c2, l2)) if l1 < l2 else ((c2, l2), (c1, l1))
            if (hi[0] >> (hi[1] - lo[1])) == lo[0]:
                raise AssertionError(f"{label}: 0x{lo[0]:x}/{lo[1]} prefixes 0x{hi[0]:x}/{hi[1]}")
    kraft = sum(2.0 ** -l for l in lens)
    assert kraft <= 1.0 + 1e-9, f"{label}: Kraft sum {kraft} > 1"


def unpack_pair(packed):
    hi = [(v >> 8) & 0xFF for v in packed]
    lo = [v & 0xFF for v in packed]
    return hi, lo


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--srcdir", default=None)
    ap.add_argument("--out", default="src/aac_tables.hpp")
    args = ap.parse_args()

    srcdir = args.srcdir or tempfile.mkdtemp(prefix="aac_tables_")
    paths = fetch(srcdir)
    vo = strip_comments(open(paths["aac_rom.c"]).read())
    ff = strip_comments(open(paths["aactab.c"]).read())

    # ---- spectral codebooks -------------------------------------------------
    sizes = [81, 81, 81, 81, 81, 81, 64, 64, 169, 169, 289]
    dims = [4, 4, 4, 4, 2, 2, 2, 2, 2, 2, 2]
    lavs = [1, 1, 2, 2, 4, 4, 7, 7, 12, 12, 16]
    signed = [1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0]

    ff_bits = [extract_array(ff, f"bits{k}") for k in range(1, 12)]
    ff_codes = [extract_array(ff, f"codes{k}") for k in range(1, 12)]

    vo_lens = [None] * 11
    for pair, (a, b) in ((extract_array(vo, "huff_ltab1_2"), (0, 1)),
                         (extract_array(vo, "huff_ltab3_4"), (2, 3)),
                         (extract_array(vo, "huff_ltab5_6"), (4, 5)),
                         (extract_array(vo, "huff_ltab7_8"), (6, 7)),
                         (extract_array(vo, "huff_ltab9_10"), (8, 9))):
        hi, lo = unpack_pair(pair)
        vo_lens[a], vo_lens[b] = hi, lo
    vo_lens[10] = extract_array(vo, "huff_ltab11")
    vo_codes = [extract_array(vo, f"huff_ctab{k}") for k in range(1, 12)]

    for k in range(11):
        assert len(ff_bits[k]) == sizes[k] == len(ff_codes[k]), f"book{k+1} ff size"
        assert len(vo_lens[k]) == sizes[k] == len(vo_codes[k]), f"book{k+1} vo size"
        assert ff_bits[k] == vo_lens[k], f"book{k+1}: lengths differ ff vs vo"
        assert ff_codes[k] == vo_codes[k], f"book{k+1}: codes differ ff vs vo"
        check_prefix_free(ff_codes[k], ff_bits[k], f"book{k+1}")

    # ---- scalefactor codebook ----------------------------------------------
    scf_bits = extract_array(ff, "ff_aac_scalefactor_bits")
    scf_codes = extract_array(ff, "ff_aac_scalefactor_code")
    vo_scf_lens = extract_array(vo, "huff_ltabscf")
    vo_scf_codes = extract_array(vo, "huff_ctabscf")
    assert len(scf_bits) == len(scf_codes) == 121
    assert scf_bits == vo_scf_lens, "scf lengths differ"
    # ffmpeg left-aligns scalefactor codes to 32 bits? No — compare raw first,
    # else right-align both before failing.
    if scf_codes != vo_scf_codes:
        aligned = [c >> (32 - l) if c >= (1 << l) else c for c, l in zip(scf_codes, scf_bits)]
        assert aligned == vo_scf_codes, "scf codes differ even after alignment"
        scf_codes = aligned
    check_prefix_free(scf_codes, scf_bits, "scf")

    # ---- scalefactor band tables -------------------------------------------
    num_swb_long = extract_array(ff, "ff_aac_num_swb_1024")[:12]
    num_swb_short = extract_array(ff, "ff_aac_num_swb_128")[:12]
    ff_long_names = ["96", "96", "64", "48", "48", "32", "24", "24", "16", "16", "16", "8"]
    ff_short_names = ["96", "96", "96", "48", "48", "48", "24", "24", "16", "16", "16", "8"]
    long_tabs, short_tabs = [], []
    for i in range(12):
        lt = extract_array(ff, f"swb_offset_1024_{ff_long_names[i]}")
        st = extract_array(ff, f"swb_offset_128_{ff_short_names[i]}")
        # normalize: ensure terminator present
        if lt[-1] != 1024:
            lt = lt + [1024]
        if st[-1] != 128:
            st = st + [128]
        assert len(lt) == num_swb_long[i] + 1, f"rate{i} long: {len(lt)} vs {num_swb_long[i]}+1"
        assert len(st) == num_swb_short[i] + 1, f"rate{i} short: {len(st)} vs {num_swb_short[i]}+1"
        long_tabs.append(lt)
        short_tabs.append(st)

    # cross-check against vo-aacenc band tables
    vo_total_long = extract_array(vo, "sfBandTotalLong")
    vo_total_short = extract_array(vo, "sfBandTotalShort")
    vo_long_off = extract_array(vo, "sfBandTabLongOffset")
    vo_long_tab = extract_array(vo, "sfBandTabLong")
    vo_short_off = extract_array(vo, "sfBandTabShortOffset")
    vo_short_tab = extract_array(vo, "sfBandTabShort")
    assert vo_total_long == num_swb_long, f"num_swb_long differ: {vo_total_long} vs {num_swb_long}"
    assert vo_total_short == num_swb_short
    for i in range(12):
        vl = vo_long_tab[vo_long_off[i] : vo_long_off[i] + num_swb_long[i] + 1]
        vs = vo_short_tab[vo_short_off[i] : vo_short_off[i] + num_swb_short[i] + 1]
        assert vl == long_tabs[i], f"rate{i}: long sfb table differs\n{vl}\n{long_tabs[i]}"
        assert vs == short_tabs[i], f"rate{i}: short sfb table differs"

    sample_rates = extract_array(vo, "sampRateTab")[:12]
    assert sample_rates == [96000, 88200, 64000, 48000, 44100, 32000,
                            24000, 22050, 16000, 12000, 11025, 8000]

    # ---- TNS ----------------------------------------------------------------
    # Max-bands limits (ffmpeg aactab.c; 13th entry is the 7350 Hz index we
    # don't support). Cross-check: the 4-bit TNS coefficient map must match
    # vo-aacenc's Q31 tnsCoeff4 table, which validates the arcsin/sine
    # (de)quantization formula the encoder implements in code:
    #   iqfac(+) = (2^(res-1) - 0.5) / (pi/2), iqfac(-) with + 0.5,
    #   coef = sin(idx / iqfac)
    import math
    tns_long = extract_array(ff, "ff_tns_max_bands_1024")[:12]
    tns_short = extract_array(ff, "ff_tns_max_bands_128")[:12]
    vo_c4 = extract_array(vo, "tnsCoeff3Borders")  # presence check only
    vo_c4 = extract_array(vo, "tnsCoeff4")
    iqp = (2 ** 3 - 0.5) / (math.pi / 2)
    iqm = (2 ** 3 + 0.5) / (math.pi / 2)
    # vo table order: idx -8..7 (two's complement order 0..7, -8..-1)? Try both
    # orders and require one to match within Q31 rounding.
    def q31(x):
        v = int(round(x * (1 << 31)))
        return max(-(1 << 31), min((1 << 31) - 1, v))
    ref_signed = [q31(math.sin(i / (iqm if i < 0 else iqp)))
                  for i in range(-8, 8)]
    ref_wrapped = [q31(math.sin(i / (iqm if i < 0 else iqp)))
                   for i in list(range(0, 8)) + list(range(-8, 0))]
    vo_signed = [v - (1 << 32) if v >= (1 << 31) else v for v in vo_c4]
    # vo computed the map with fixed-point sine: agree to ~100 Q31 ULPs.
    ok = any(all(abs(a - b) <= 256 for a, b in zip(vo_signed, ref))
             for ref in (ref_signed, ref_wrapped))
    assert ok, "TNS coef formula does not reproduce vo-aacenc tnsCoeff4"

    print("all cross-checks passed: 11 spectral books, scf book, 12 sfb tables, TNS coef map")

    # ---- emit ---------------------------------------------------------------
    def fmt(vals, per_line=12, hexfmt=False):
        out, line = [], []
        for v in vals:
            line.append(f"0x{v:x}" if hexfmt else str(v))
            if len(line) == per_line:
                out.append("    " + ", ".join(line) + ",")
                line = []
        if line:
            out.append("    " + ", ".join(line) + ",")
        return "\n".join(out)

    h = []
    h.append("// glint - AAC standard tables (GENERATED FILE — do not edit)")
    h.append("// Regenerate with: python tools/gen_aac_tables.py")
    h.append("//")
    h.append("// Normative data from ISO/IEC 13818-7 / 14496-3 (scalefactor-band offsets,")
    h.append("// spectral Huffman codebooks 1-11, scalefactor codebook). Values extracted")
    h.append("// from two independent implementations (vo-aacenc, Apache-2.0; ffmpeg")
    h.append("// libavcodec/aactab.c) and cross-checked bit-for-bit by the generator.")
    h.append("")
    h.append("#ifndef GLINT_AAC_TABLES_HPP")
    h.append("#define GLINT_AAC_TABLES_HPP")
    h.append("")
    h.append("#include <cstdint>")
    h.append("")
    h.append("namespace glint {")
    h.append("namespace aac_tables {")
    h.append("")
    h.append("constexpr int kNumSampleRates = 12;")
    h.append("inline constexpr int kSampleRates[kNumSampleRates] = {")
    h.append(fmt(sample_rates))
    h.append("};")
    h.append("")
    h.append("inline constexpr uint8_t kNumSwbLong[kNumSampleRates] = {")
    h.append(fmt(num_swb_long))
    h.append("};")
    h.append("inline constexpr uint8_t kNumSwbShort[kNumSampleRates] = {")
    h.append(fmt(num_swb_short))
    h.append("};")
    h.append("")
    emitted = {}
    long_names = []
    for i in range(12):
        key = tuple(long_tabs[i])
        if key not in emitted:
            nm = f"kSwbLong_{sample_rates[i]}"
            emitted[key] = nm
            h.append(f"inline constexpr uint16_t {nm}[{len(long_tabs[i])}] = {{")
            h.append(fmt(long_tabs[i]))
            h.append("};")
        long_names.append(emitted[key])
    h.append("inline constexpr const uint16_t* kSwbOffsetLong[kNumSampleRates] = {")
    h.append("    " + ", ".join(long_names) + ",")
    h.append("};")
    h.append("")
    emitted = {}
    short_names = []
    for i in range(12):
        key = tuple(short_tabs[i])
        if key not in emitted:
            nm = f"kSwbShort_{sample_rates[i]}"
            emitted[key] = nm
            h.append(f"inline constexpr uint16_t {nm}[{len(short_tabs[i])}] = {{")
            h.append(fmt(short_tabs[i]))
            h.append("};")
        short_names.append(emitted[key])
    h.append("inline constexpr const uint16_t* kSwbOffsetShort[kNumSampleRates] = {")
    h.append("    " + ", ".join(short_names) + ",")
    h.append("};")
    h.append("")
    h.append("// Spectral Huffman codebooks 1-11 (index 0 = book 1).")
    h.append("// dim: coefficients per codeword; lav: largest absolute value;")
    h.append("// signed: table indices span [-lav, lav] (no sign bits) vs [0, lav] + sign bits.")
    h.append("inline constexpr uint8_t kBookDim[11]    = { " + ", ".join(map(str, dims)) + " };")
    h.append("inline constexpr uint8_t kBookLav[11]    = { " + ", ".join(map(str, lavs)) + " };")
    h.append("inline constexpr uint8_t kBookSigned[11] = { " + ", ".join(map(str, signed)) + " };")
    h.append("inline constexpr uint16_t kBookSize[11]  = { " + ", ".join(map(str, sizes)) + " };")
    h.append("")
    for k in range(11):
        h.append(f"inline constexpr uint8_t kSpecBits{k+1}[{sizes[k]}] = {{")
        h.append(fmt(ff_bits[k], 16))
        h.append("};")
        h.append(f"inline constexpr uint16_t kSpecCodes{k+1}[{sizes[k]}] = {{")
        h.append(fmt(ff_codes[k], 8, hexfmt=True))
        h.append("};")
    h.append("")
    h.append("inline constexpr const uint8_t* kSpecBits[11] = {")
    h.append("    " + ", ".join(f"kSpecBits{k+1}" for k in range(11)) + ",")
    h.append("};")
    h.append("inline constexpr const uint16_t* kSpecCodes[11] = {")
    h.append("    " + ", ".join(f"kSpecCodes{k+1}" for k in range(11)) + ",")
    h.append("};")
    h.append("")
    h.append("// TNS: max scalefactor bands the filter may cover (long/short windows).")
    h.append("inline constexpr uint8_t kTnsMaxBandsLong[kNumSampleRates] = {")
    h.append(fmt(tns_long))
    h.append("};")
    h.append("inline constexpr uint8_t kTnsMaxBandsShort[kNumSampleRates] = {")
    h.append(fmt(tns_short))
    h.append("};")
    h.append("")
    h.append("// Scalefactor codebook: index = dpcm + 60, dpcm in [-60, 60].")
    h.append("inline constexpr uint8_t kScfBits[121] = {")
    h.append(fmt(scf_bits, 16))
    h.append("};")
    h.append("inline constexpr uint32_t kScfCodes[121] = {")
    h.append(fmt(scf_codes, 8, hexfmt=True))
    h.append("};")
    h.append("")
    h.append("} // namespace aac_tables")
    h.append("} // namespace glint")
    h.append("")
    h.append("#endif // GLINT_AAC_TABLES_HPP")

    with open(args.out, "w") as f:
        f.write("\n".join(h))
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
