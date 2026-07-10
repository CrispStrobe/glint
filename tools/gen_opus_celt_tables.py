#!/usr/bin/env python3
"""Generate src/opus_celt_tables.hpp — CELT static-mode tables (48 kHz / 960).

The CELT decoder (PLAN.md § O1) needs the static mode data libopus builds at
init time (opus-1.5.2, celt/static_modes_float.h + celt/modes.c) plus the
coarse-energy probability model (celt/quant_bands.c) and the small icdf
tables the frame parser reads (celt/celt.h). This script extracts them from
the libopus 1.5.2 source text, cross-checks everything it can against an
independent formulation — the analytic Vorbis-power window for window120,
the FIXED_POINT Q15/Q4 twins for pred_coef/beta_coef/beta_intra/eMeans,
modes.c's BITALLOC_SIZE for the allocation-table row count, and the declared
C array dimensions against the parsed element counts — and only then emits
the C++ header. Any missing table or unexpected length is a hard failure
(nonzero exit). Run from the repo root:

    python tools/gen_opus_celt_tables.py [--srcdir DIR]

If --srcdir is not given, the libopus source tree is expected at
~/code/glint-tools/opus-1.5.2 (https://downloads.xiph.org/releases/opus/).
"""

import argparse
import math
import os
import re
import sys

DEFAULT_SRCDIR = os.path.expanduser("~/code/glint-tools/opus-1.5.2")


def fail(msg):
    sys.stderr.write(f"gen_opus_celt_tables: FATAL: {msg}\n")
    sys.exit(1)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def find_initializers(text, name):
    """All `name [dims]* = { ... }` initializers: list of (declared_size, body).

    declared_size is the product of the declared dimensions, or None if any
    dimension is empty (e.g. `eband5ms[]`).
    """
    out = []
    for m in re.finditer(r"\b" + re.escape(name) + r"\s*((?:\[[^\]]*\]\s*)*)=\s*\{", text):
        dims = re.findall(r"\[([^\]]*)\]", m.group(1))
        size = 1
        for d in dims:
            d = d.strip()
            if not d:
                size = None
                break
            size = size * int(d, 0) if size is not None else None
        depth, i = 1, m.end()
        while depth > 0:
            if i >= len(text):
                fail(f"{name}: unbalanced braces")
            c = text[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            i += 1
        out.append((size, text[m.end(): i - 1]))
    return out


def parse_ints(body):
    return [int(tok, 0) for tok in re.findall(r"-?(?:0[xX][0-9a-fA-F]+|\d+)", body)]


def parse_float_literals(body):
    """Float literal STRINGS as written in the source ('f' suffix stripped)."""
    toks = re.findall(r"[-+]?(?:\d+\.\d*|\.\d+|\d+)(?:[eE][-+]?\d+)?f?", body)
    return [t.rstrip("fF") for t in toks]


def extract_ints(text, name, want_len=None, which=0, label=None):
    inits = find_initializers(text, name)
    if len(inits) <= which:
        fail(f"table '{name}' not found (occurrence {which}, got {len(inits)})")
    size, body = inits[which]
    vals = parse_ints(body)
    if size is not None and len(vals) != size:
        fail(f"{name}: parsed {len(vals)} values but declaration says {size}")
    if want_len is not None and len(vals) != want_len:
        fail(f"{label or name}: expected {want_len} values, got {len(vals)}")
    return vals


def pick_int_type(vals, name):
    lo, hi = min(vals), max(vals)
    if lo >= 0 and hi <= 0xFF:
        return "uint8_t"
    if -0x8000 <= lo and hi <= 0x7FFF:
        return "int16_t"
    fail(f"{name}: values [{lo}, {hi}] do not fit uint8_t/int16_t")


def fmt(vals, per_line=15):
    out, line = [], []
    for v in vals:
        line.append(str(v))
        if len(line) == per_line:
            out.append("    " + ", ".join(line) + ",")
            line = []
    if line:
        out.append("    " + ", ".join(line) + ",")
    return "\n".join(out)


def check_icdf(name, vals):
    if vals[0] >= 256:
        fail(f"{name}: first entry {vals[0]} >= 256")
    if vals[-1] != 0:
        fail(f"{name}: does not end at 0")
    for a, b in zip(vals, vals[1:]):
        if b >= a:
            fail(f"{name}: not strictly decreasing ({a} -> {b})")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--srcdir", default=DEFAULT_SRCDIR)
    ap.add_argument("--out", default="src/opus_celt_tables.hpp")
    args = ap.parse_args()

    celt_dir = os.path.join(args.srcdir, "celt")
    srcs = {}
    for fn in ("static_modes_float.h", "modes.c", "quant_bands.c", "celt.h"):
        p = os.path.join(celt_dir, fn)
        if not os.path.exists(p):
            fail(f"missing {p} — extract opus-1.5.2 there or pass --srcdir")
        srcs[fn] = strip_comments(open(p).read())

    smf = srcs["static_modes_float.h"]
    modes = srcs["modes.c"]
    qb = srcs["quant_bands.c"]
    celth = srcs["celt.h"]

    summary = []

    def note(name, n, extra=""):
        summary.append(f"  {name:18s} len {n:4d}  {extra}".rstrip())

    # ---- eband5ms (modes.c) — band edges in 200 Hz units --------------------
    eband5ms = extract_ints(modes, "eband5ms", want_len=22)
    if eband5ms[0] != 0 or eband5ms[-1] != 100:
        fail(f"eband5ms: expected 0..100 span, got {eband5ms[0]}..{eband5ms[-1]}")
    for a, b in zip(eband5ms, eband5ms[1:]):
        if b <= a:
            fail(f"eband5ms: not strictly increasing ({a} -> {b})")
    note("eband5ms", 22, "strictly increasing, 0..100")

    # ---- band_allocation (modes.c) — cross-check rows vs BITALLOC_SIZE ------
    band_alloc = extract_ints(modes, "band_allocation")
    if len(band_alloc) % 21 != 0:
        fail(f"band_allocation: length {len(band_alloc)} not divisible by 21")
    n_alloc_vectors = len(band_alloc) // 21
    m = re.search(r"#define\s+BITALLOC_SIZE\s+(\d+)", modes)
    if not m or int(m.group(1)) != n_alloc_vectors:
        fail(f"band_allocation rows {n_alloc_vectors} != BITALLOC_SIZE "
             f"{m.group(1) if m else '<missing>'}")
    note("band_allocation", len(band_alloc), f"= {n_alloc_vectors} rows x 21 bands")

    # ---- static mode tables (static_modes_float.h) ---------------------------
    logn400 = extract_ints(smf, "logN400", want_len=21)
    cache_index50 = extract_ints(smf, "cache_index50", want_len=105)
    cache_bits50 = extract_ints(smf, "cache_bits50", want_len=392)
    cache_caps50 = extract_ints(smf, "cache_caps50", want_len=168)
    for v in cache_index50:
        if v != -1 and not (0 <= v < len(cache_bits50)):
            fail(f"cache_index50: entry {v} is neither -1 nor a valid index "
                 f"< {len(cache_bits50)}")
    note("logN400", 21)
    note("cache_index50", 105, f"entries -1 or [0, {len(cache_bits50)})")
    note("cache_bits50", 392)
    note("cache_caps50", 168, "= 21 bands x 2 channels x 4 LM")

    # ---- window120: literals preserved, checked against the analytic form ---
    inits = find_initializers(smf, "window120")
    if not inits:
        fail("window120 not found")
    size, body = inits[0]
    win_lits = parse_float_literals(body)
    if size != 120 or len(win_lits) != 120:
        fail(f"window120: expected 120 values, got {len(win_lits)} (decl {size})")
    win = [float(t) for t in win_lits]

    def delta(f):
        return max(abs(w - f(n)) for n, w in enumerate(win))

    formula_a = lambda n: math.sin(0.5 * math.pi
                                   * math.sin(0.5 * math.pi * (n + 0.5) / 120) ** 2)
    formula_b = lambda n: math.sin(0.5 * math.pi
                                   * math.sin(0.5 * math.pi * (n + 0.5) / 120)) ** 2
    da, db = delta(formula_a), delta(formula_b)
    if da < 1e-4:
        win_formula, win_delta = "sin(pi/2 * sin(pi/2*(n+0.5)/120)^2)", da
    elif db < 1e-4:
        win_formula, win_delta = "sin(pi/2 * sin(pi/2*(n+0.5)/120))^2", db
    else:
        fail(f"window120 matches neither analytic form (deltas {da:.3g}, {db:.3g})")
    note("window120", 120, f"max |delta| vs {win_formula} = {win_delta:.3g}")

    # ---- coarse-energy model (quant_bands.c) ---------------------------------
    e_prob = extract_ints(qb, "e_prob_model", want_len=4 * 2 * 42)
    for v in e_prob:
        if not (0 <= v <= 255):
            fail(f"e_prob_model: value {v} out of uint8 range")
    note("e_prob_model", len(e_prob), "= 4 LM x 2 (inter/intra) x 21 bands x 2")

    # eMeans: FIXED_POINT Q4 twin cross-checks the float branch exactly.
    em_inits = find_initializers(qb, "eMeans")
    if len(em_inits) != 2:
        fail(f"eMeans: expected fixed+float twin definitions, got {len(em_inits)}")
    em_q4 = parse_ints(em_inits[0][1])
    em_float_lits = parse_float_literals(em_inits[1][1])
    if len(em_q4) != 25 or len(em_float_lits) != 25:
        fail(f"eMeans: expected 25 entries, got {len(em_q4)}/{len(em_float_lits)}")
    for q, lit in zip(em_q4, em_float_lits):
        if float(lit) != q / 16.0:
            fail(f"eMeans: float branch {lit} != Q4 branch {q}/16")
    note("eMeans", 25, "float == Q4/16 cross-checked; mode uses first 21")

    # pred/beta coefficients: parse the Q15 integers (exact), cross-check the
    # float branch's <int>/32768. expressions carry the same numerators.
    def q15_pair(name, count):
        inits2 = find_initializers(qb, name)
        if len(inits2) != 2:
            fail(f"{name}: expected fixed+float twin definitions, got {len(inits2)}")
        q15 = parse_ints(inits2[0][1])
        nums = [int(t) for t in re.findall(r"(\d+)\s*/\s*32768\.", inits2[1][1])]
        if len(q15) != count or nums != q15:
            fail(f"{name}: Q15 {q15} vs float-branch numerators {nums}")
        return q15

    pred_q15 = q15_pair("pred_coef", 4)
    beta_q15 = q15_pair("beta_coef", 4)
    mfix = re.search(r"beta_intra\s*=\s*(\d+)\s*;", qb)
    mflt = re.search(r"beta_intra\s*=\s*(\d+)\s*/\s*32768\.\s*;", qb)
    if not mfix or not mflt or mfix.group(1) != mflt.group(1):
        fail("beta_intra: fixed/float twin definitions disagree or missing")
    beta_intra_q15 = int(mfix.group(1))
    note("pred_coef", 4, f"Q15 {pred_q15}")
    note("beta_coef", 4, f"Q15 {beta_q15}")
    note("beta_intra", 1, f"Q15 {beta_intra_q15}")

    # ---- icdf tables ----------------------------------------------------------
    icdfs = {
        "small_energy_icdf": extract_ints(qb, "small_energy_icdf", want_len=3),
        "tapset_icdf": extract_ints(celth, "tapset_icdf", want_len=3),
        "spread_icdf": extract_ints(celth, "spread_icdf", want_len=4),
        "trim_icdf": extract_ints(celth, "trim_icdf", want_len=11),
    }
    for name, vals in icdfs.items():
        check_icdf(name, vals)
        note(name, len(vals), "strictly decreasing, ends 0")

    print("all cross-checks passed:")
    print("\n".join(summary))

    # ---- emit -----------------------------------------------------------------
    h = []
    h.append("// glint - Opus CELT static-mode tables (GENERATED FILE — do not edit)")
    h.append("// Regenerate with: python tools/gen_opus_celt_tables.py")
    h.append("//")
    h.append("// The 48 kHz / 960-sample static CELT mode from libopus 1.5.2 (BSD):")
    h.append("// celt/static_modes_float.h, celt/modes.c, celt/quant_bands.c, celt/celt.h.")
    h.append("// The generator cross-checks declared C dimensions vs parsed counts, the")
    h.append("// window against its analytic form, the coarse-energy coefficients against")
    h.append("// their FIXED_POINT Q15/Q4 twins, and icdf monotonicity before emitting.")
    h.append(f"// window120 == {win_formula}, max |delta| {win_delta:.3g}.")
    h.append("")
    h.append("#ifndef GLINT_OPUS_CELT_TABLES_HPP")
    h.append("#define GLINT_OPUS_CELT_TABLES_HPP")
    h.append("")
    h.append("#include <cstdint>")
    h.append("")
    h.append("namespace glint {")
    h.append("namespace opus {")
    h.append("namespace celt {")
    h.append("")
    h.append("constexpr int kNbEBands = 21;       // energy bands in the 48k/960 mode")
    h.append("constexpr int kOverlap = 120;       // MDCT window overlap (= short MDCT)")
    h.append("constexpr int kMaxLM = 3;           // frame = kShortMdctSize << LM, LM 0..3")
    h.append("constexpr int kShortMdctSize = 120; // 2.5 ms at 48 kHz")
    h.append(f"constexpr int kNbAllocVectors = {n_alloc_vectors};  // rows of kBandAllocation")
    h.append("")
    h.append("// Band edges in units of 4 MDCT bins at LM 0 (multiply by 1<<LM);")
    h.append("// kEBands[b] .. kEBands[b+1] spans band b, 22 entries for 21 bands.")
    h.append(f"inline constexpr {pick_int_type(eband5ms, 'eband5ms')} "
             f"kEBands[kNbEBands + 1] = {{")
    h.append(fmt(eband5ms))
    h.append("};")
    h.append("")
    h.append("// Allocation table, 1/32 bit per sample per band; rows = quality lines")
    h.append("// interpolated by the allocator (celt/modes.c band_allocation).")
    h.append(f"inline constexpr {pick_int_type(band_alloc, 'band_allocation')} "
             f"kBandAllocation[kNbAllocVectors * kNbEBands] = {{")
    h.append(fmt(band_alloc, per_line=21))
    h.append("};")
    h.append("")
    h.append("// log2 of the effective band width, Q(BITRES=3) (static_modes logN400).")
    h.append(f"inline constexpr {pick_int_type(logn400, 'logN400')} "
             f"kLogN[kNbEBands] = {{")
    h.append(fmt(logn400))
    h.append("};")
    h.append("")
    h.append("// MDCT half-window (Vorbis power window), 120 samples; the source float")
    h.append("// literals are preserved verbatim (double holds them exactly).")
    h.append("inline constexpr double kWindow[kOverlap] = {")
    h.append(fmt(win_lits, per_line=5))
    h.append("};")
    h.append("")
    h.append("// PVQ pulse cache (celt/rate.h bits2pulses): kCacheIndex[(LM+1)*21 + band]")
    h.append("// is -1 (band too small at that LM) or an offset into kCacheBits; the")
    h.append("// entry at the offset is the cache length, followed by that many bytes.")
    h.append(f"inline constexpr {pick_int_type(cache_index50, 'cache_index50')} "
             f"kCacheIndex[{len(cache_index50)}] = {{")
    h.append(fmt(cache_index50))
    h.append("};")
    h.append(f"inline constexpr {pick_int_type(cache_bits50, 'cache_bits50')} "
             f"kCacheBits[{len(cache_bits50)}] = {{")
    h.append(fmt(cache_bits50))
    h.append("};")
    h.append("// Max pseudo-pulses per band, Q(BITRES): kCacheCaps[(LM*2 + C-1)*21 + band].")
    h.append(f"inline constexpr {pick_int_type(cache_caps50, 'cache_caps50')} "
             f"kCacheCaps[{len(cache_caps50)}] = {{")
    h.append(fmt(cache_caps50, per_line=21))
    h.append("};")
    h.append("")
    h.append("// Coarse-energy Laplace model, Q8 (probability-of-zero, decay) pairs per")
    h.append("// band: kEProbModel[LM][intra][band*2 + {0,1}] (celt/quant_bands.c).")
    h.append("inline constexpr uint8_t kEProbModel[4][2][42] = {")
    for lm in range(4):
        h.append("    {")
        for intra in range(2):
            chunk = e_prob[(lm * 2 + intra) * 42:(lm * 2 + intra + 1) * 42]
            h.append("        {")
            h.append("\n".join("        " + ln for ln in fmt(chunk, per_line=14).split("\n")))
            h.append("        },")
        h.append("    },")
    h.append("};")
    h.append("")
    h.append("// Mean band energy (Q4 quantized, exact in double). The 48k/960 mode uses")
    h.append("// the first kNbEBands entries; 25 kept to match the source array.")
    h.append("inline constexpr double kEMeans[25] = {")
    h.append(fmt(em_float_lits, per_line=5))
    h.append("};")
    h.append("")
    h.append("// Inter-frame energy prediction / decay, exact Q15 values (per LM).")
    h.append("inline constexpr double kPredCoef[4] = {")
    h.append("    " + ", ".join(f"{v}/32768.0" for v in pred_q15) + ",")
    h.append("};")
    h.append("inline constexpr double kBetaCoef[4] = {")
    h.append("    " + ", ".join(f"{v}/32768.0" for v in beta_q15) + ",")
    h.append("};")
    h.append(f"inline constexpr double kBetaIntra = {beta_intra_q15}/32768.0;")
    h.append("")
    h.append("// icdf tables for ec_dec_icdf (decreasing, end at 0; ftb in comment).")
    h.append("inline constexpr uint8_t kSmallEnergyIcdf[3] = { "
             + ", ".join(map(str, icdfs["small_energy_icdf"])) + " };  // ftb 2")
    h.append("inline constexpr uint8_t kTapsetIcdf[3] = { "
             + ", ".join(map(str, icdfs["tapset_icdf"])) + " };  // ftb 2")
    h.append("inline constexpr uint8_t kSpreadIcdf[4] = { "
             + ", ".join(map(str, icdfs["spread_icdf"])) + " };  // ftb 5")
    h.append("inline constexpr uint8_t kTrimIcdf[11] = { "
             + ", ".join(map(str, icdfs["trim_icdf"])) + " };  // ftb 7")
    h.append("")
    h.append("}  // namespace celt")
    h.append("}  // namespace opus")
    h.append("}  // namespace glint")
    h.append("")
    h.append("#endif  // GLINT_OPUS_CELT_TABLES_HPP")

    with open(args.out, "w") as f:
        f.write("\n".join(h) + "\n")
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
