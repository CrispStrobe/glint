#!/usr/bin/env python3
"""Generate src/opus_silk_tables.hpp — SILK decoder tables from libopus 1.5.2.

The SILK decoder (PLAN.md § O2) needs the static tables libopus keeps in
silk/tables_*.c, silk/pitch_est_tables.c, silk/table_LSF_cos.c and
silk/resampler_rom.{c,h}. This script extracts them from the libopus 1.5.2
source text and cross-checks everything it can before emitting:

  * every parsed element count against the array dimensions declared at the
    definition site (macro dimensions are resolved from define.h /
    pitch_est_defines.h / resampler_rom.h);
  * the definition-site dimensions against the independent extern
    declarations in tables.h / pitch_est_defines.h / resampler_rom.h;
  * every value against the declared opus_{u,}int{8,16,32} range;
  * every iCDF (row) for SILK's convention: uint8, first entry < 256,
    non-increasing (strictly, with repeats reported), final element 0 —
    silk_sign_iCDF is exempt (it stores single Q8 probabilities, not iCDFs);
  * the shell-code tables row-by-row via silk_shell_code_table_offsets;
  * the LTP codebooks against silk_LTP_vq_sizes and the pointer-table order;
  * the NLSF codebook structs field-by-field against structs.h and the shape
    invariants nVectors*order == len(CB1) == len(CB1_Wght),
    len(CB1_iCDF) == 2*nVectors, len(pred_Q8) == 2*(order-1),
    len(ec_sel) == nVectors*order/2,
    len(ec_iCDF) == len(ec_Rates_Q5) == 8*(2*NLSF_QUANT_MAX_AMPLITUDE+1),
    len(deltaMin_Q15) == order+1, with SILK_FIX_CONST evaluated exactly.

Any missing table, dimension mismatch or range violation is a hard failure
(nonzero exit). Run from the repo root:

    python tools/gen_opus_silk_tables.py [--srcdir DIR]

If --srcdir is not given, the libopus source tree is expected at
~/code/glint-tools/opus-1.5.2 (https://downloads.xiph.org/releases/opus/).
"""

import argparse
import os
import re
import sys

DEFAULT_SRCDIR = os.path.expanduser("~/code/glint-tools/opus-1.5.2")

TYPE_MAP = {
    "opus_uint8": ("uint8_t", 0, 255, 1),
    "opus_int8": ("int8_t", -128, 127, 1),
    "opus_int16": ("int16_t", -32768, 32767, 2),
    "opus_int32": ("int32_t", -(1 << 31), (1 << 31) - 1, 4),
}

NAME_OVERRIDES = {
    "silk_LSFCosTab_FIX_Q12": "kLsfCosTabFixQ12",
    "silk_LTPscale_iCDF": "kLtpScaleIcdf",
    "silk_LTPScales_table_Q14": "kLtpScalesTableQ14",
    "silk_CB_lags_stage2_10_ms": "kCbLagsStage2_10ms",
    "silk_CB_lags_stage3_10_ms": "kCbLagsStage3_10ms",
    "silk_resampler_down2_0": "kResamplerDown2Coef0",
    "silk_resampler_down2_1": "kResamplerDown2Coef1",
}


def fail(msg):
    sys.stderr.write(f"gen_opus_silk_tables: FATAL: {msg}\n")
    sys.exit(1)


def strip_comments(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def camel(cname):
    if cname in NAME_OVERRIDES:
        return NAME_OVERRIDES[cname]
    parts = cname.split("_")
    if parts[0] != "silk":
        fail(f"unexpected table name {cname} (no silk_ prefix)")
    return "k" + "".join(p if p.isdigit() else p[:1].upper() + p[1:].lower()
                         for p in parts[1:] if p)


# --------------------------------------------------------------------------
# macro resolution
# --------------------------------------------------------------------------

def parse_defines(text, into):
    for m in re.finditer(r"^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+([^\n]+)",
                         text, re.M):
        name, val = m.group(1), m.group(2).strip()
        if "(" in name:
            continue
        into.setdefault(name, val)


def eval_expr(expr, macros, depth=0):
    """Evaluate an integer C constant expression using the macro table."""
    if depth > 32:
        fail(f"macro recursion too deep evaluating {expr!r}")
    expr = expr.strip()
    m = re.fullmatch(r"-?\d+", expr)
    if m:
        return int(expr)
    missing = []

    def repl(tok):
        name = tok.group(0)
        if name in macros:
            return "(" + str(eval_expr(macros[name], macros, depth + 1)) + ")"
        missing.append(name)
        return name

    subst = re.sub(r"[A-Za-z_]\w*", repl, expr)
    if missing:
        fail(f"unknown identifier(s) {missing} in expression {expr!r}")
    try:
        val = eval(subst, {"__builtins__": {}}, {})  # arithmetic only
    except Exception as e:
        fail(f"cannot evaluate {expr!r} -> {subst!r}: {e}")
    ival = int(val)
    if ival != val:
        fail(f"expression {expr!r} is not integral (= {val})")
    return ival


# --------------------------------------------------------------------------
# C array / scalar parsing
# --------------------------------------------------------------------------

def parse_arrays(text, macros, into, srcname):
    """All `[static] [silk_DWORD_ALIGN] const TYPE name[dims]* = {...};`."""
    pat = re.compile(
        r"\b(?:static\s+)?(?:silk_DWORD_ALIGN\s+)?(?:static\s+)?const\s+"
        r"(opus_(?:u?int8|int16|int32))\s+(\w+)\s*((?:\[[^\]]*\]\s*)+)=\s*\{")
    for m in pat.finditer(text):
        ctype, name, dimtext = m.group(1), m.group(2), m.group(3)
        dims = [eval_expr(d, macros) for d in re.findall(r"\[([^\]]+)\]", dimtext)]
        depth, i = 1, m.end()
        while depth > 0:
            if i >= len(text):
                fail(f"{name}: unbalanced braces in {srcname}")
            c = text[i]
            if c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            i += 1
        body = text[m.end(): i - 1]
        toks = [t.strip() for t in body.replace("{", " ").replace("}", " ").split(",")]
        vals = [eval_expr(t, macros) for t in toks if t]
        want = 1
        for d in dims:
            want *= d
        if len(vals) != want:
            fail(f"{name} ({srcname}): parsed {len(vals)} values, "
                 f"declared dims {dims} = {want}")
        if name in into:
            fail(f"{name}: duplicate definition ({srcname})")
        into[name] = {"ctype": ctype, "dims": dims, "vals": vals, "src": srcname}


def parse_scalars(text, macros, into, srcname):
    """`static const TYPE name = expr;` (no brackets)."""
    pat = re.compile(r"\bstatic\s+const\s+(opus_(?:u?int8|int16|int32))\s+"
                     r"(\w+)\s*=\s*([^;{\[]+);")
    for m in pat.finditer(text):
        into[m.group(2)] = {"ctype": m.group(1),
                            "val": eval_expr(m.group(3), macros),
                            "src": srcname}


def parse_externs(text, macros, into):
    pat = re.compile(r"extern\s+const\s+(opus_(?:u?int8|int16|int32))\s+"
                     r"(\w+)\s*((?:\[[^\]]*\]\s*)+);")
    for m in pat.finditer(text):
        dims = [eval_expr(d, macros) for d in re.findall(r"\[([^\]]+)\]", m.group(3))]
        into[m.group(2)] = {"ctype": m.group(1), "dims": dims}


def parse_ptr_table_names(text, name, count):
    """Referenced silk_* identifiers, in order, in a pointer-table initializer."""
    m = re.search(r"\b" + re.escape(name) + r"\s*\[[^\]]*\]\s*=\s*\{(.*?)\};",
                  text, re.S)
    if not m:
        fail(f"pointer table {name} not found")
    names = re.findall(r"silk_\w+", m.group(1))
    if len(names) != count:
        fail(f"{name}: expected {count} entries, got {names}")
    return names


# --------------------------------------------------------------------------
# checks
# --------------------------------------------------------------------------

def check_range(name, tab):
    _, lo, hi, _ = TYPE_MAP[tab["ctype"]]
    for v in tab["vals"]:
        if not (lo <= v <= hi):
            fail(f"{name}: value {v} outside {tab['ctype']} range [{lo}, {hi}]")


def check_icdf_row(name, row, repeats):
    """SILK iCDF row: uint8, non-increasing, ends at 0. Returns repeat count."""
    if row[0] >= 256:
        fail(f"{name}: first iCDF entry {row[0]} >= 256")
    if row[-1] != 0:
        fail(f"{name}: iCDF does not end at 0 (last = {row[-1]})")
    n_rep = 0
    for a, b in zip(row, row[1:]):
        if b > a:
            fail(f"{name}: iCDF increases ({a} -> {b})")
        if b == a:
            n_rep += 1
    if n_rep:
        repeats.append((name, n_rep))
    return n_rep


# --------------------------------------------------------------------------
# emission helpers
# --------------------------------------------------------------------------

def fmt(vals, per_line=15, indent="    "):
    out, line = [], []
    for v in vals:
        line.append(str(v))
        if len(line) == per_line:
            out.append(indent + ", ".join(line) + ",")
            line = []
    if line:
        out.append(indent + ", ".join(line) + ",")
    return "\n".join(out)


def per_line_for(ctype):
    return {"uint8_t": 15, "int8_t": 15, "int16_t": 10, "int32_t": 6}[ctype]


def emit_array(h, tab, name_c, comment_lines):
    cpp_t = TYPE_MAP[tab["ctype"]][0]
    kname = camel(name_c)
    dims = tab["dims"]
    vals = tab["vals"]
    for cl in comment_lines:
        if cl:
            h.append(f"// {cl}")
    dimtxt = "".join(f"[{d}]" for d in dims)
    h.append(f"inline constexpr {cpp_t} {kname}{dimtxt} = {{")
    if len(dims) == 1:
        h.append(fmt(vals, per_line_for(cpp_t)))
    elif len(dims) == 2:
        rows, cols = dims
        for r in range(rows):
            row = vals[r * cols:(r + 1) * cols]
            if cols <= 12:
                h.append("    { " + ", ".join(str(v) for v in row) + " },")
            else:
                h.append("    {")
                h.append(fmt(row, per_line_for(cpp_t), indent="        "))
                h.append("    },")
    else:
        fail(f"{name_c}: unsupported rank {len(dims)}")
    h.append("};")
    return kname


def nbytes(tab):
    return len(tab["vals"]) * TYPE_MAP[tab["ctype"]][3]


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--srcdir", default=DEFAULT_SRCDIR)
    ap.add_argument("--out", default="src/opus_silk_tables.hpp")
    args = ap.parse_args()

    silk_dir = os.path.join(args.srcdir, "silk")
    files = ("define.h", "pitch_est_defines.h", "resampler_rom.h", "structs.h",
             "tables.h", "tables_gain.c", "tables_LTP.c",
             "tables_NLSF_CB_NB_MB.c", "tables_NLSF_CB_WB.c", "tables_other.c",
             "tables_pitch_lag.c", "tables_pulses_per_block.c",
             "table_LSF_cos.c", "pitch_est_tables.c", "resampler_rom.c")
    src = {}
    for fn in files:
        p = os.path.join(silk_dir, fn)
        if not os.path.exists(p):
            fail(f"missing {p} — extract opus-1.5.2 there or pass --srcdir")
        src[fn] = strip_comments(open(p).read())

    macros = {}
    for fn in ("define.h", "pitch_est_defines.h", "resampler_rom.h"):
        parse_defines(src[fn], macros)

    tables, scalars, externs = {}, {}, {}
    for fn in files:
        if fn.endswith(".c") or fn == "resampler_rom.h":
            parse_arrays(src[fn], macros, tables, fn)
    parse_scalars(src["resampler_rom.h"], macros, scalars, "resampler_rom.h")
    for fn in ("tables.h", "pitch_est_defines.h", "resampler_rom.h"):
        parse_externs(src[fn], macros, externs)

    # ---- table spec: (C name, expected dims expr list, icdf row length) ----
    # icdf spec: None = not an iCDF; "rows" = last dim is one iCDF per row;
    # an int = flat table of concatenated iCDFs of that length; "shell" =
    # rows located via silk_shell_code_table_offsets.
    E = lambda e: eval_expr(str(e), macros)
    spec = [
        # tables_gain.c
        ("silk_gain_iCDF", ["3", "N_LEVELS_QGAIN/8"], "rows"),
        ("silk_delta_gain_iCDF",
         ["MAX_DELTA_GAIN_QUANT - MIN_DELTA_GAIN_QUANT + 1"], "rows"),
        # tables_pitch_lag.c
        ("silk_pitch_lag_iCDF",
         ["2*(PITCH_EST_MAX_LAG_MS - PITCH_EST_MIN_LAG_MS)"], "rows"),
        ("silk_pitch_delta_iCDF", ["21"], "rows"),
        ("silk_pitch_contour_iCDF", ["34"], "rows"),
        ("silk_pitch_contour_NB_iCDF", ["11"], "rows"),
        ("silk_pitch_contour_10_ms_iCDF", ["12"], "rows"),
        ("silk_pitch_contour_10_ms_NB_iCDF", ["3"], "rows"),
        # pitch_est_tables.c (decode_pitch.c contour codebooks)
        ("silk_CB_lags_stage2", ["PE_MAX_NB_SUBFR", "PE_NB_CBKS_STAGE2_EXT"], None),
        ("silk_CB_lags_stage3", ["PE_MAX_NB_SUBFR", "PE_NB_CBKS_STAGE3_MAX"], None),
        ("silk_CB_lags_stage2_10_ms",
         ["PE_MAX_NB_SUBFR >> 1", "PE_NB_CBKS_STAGE2_10MS"], None),
        ("silk_CB_lags_stage3_10_ms",
         ["PE_MAX_NB_SUBFR >> 1", "PE_NB_CBKS_STAGE3_10MS"], None),
        # tables_pulses_per_block.c
        ("silk_max_pulses_table", ["4"], None),
        ("silk_pulses_per_block_iCDF",
         ["N_RATE_LEVELS", "SILK_MAX_PULSES + 2"], "rows"),
        ("silk_rate_levels_iCDF", ["2", "N_RATE_LEVELS - 1"], "rows"),
        ("silk_shell_code_table0", ["152"], "shell"),
        ("silk_shell_code_table1", ["152"], "shell"),
        ("silk_shell_code_table2", ["152"], "shell"),
        ("silk_shell_code_table3", ["152"], "shell"),
        ("silk_shell_code_table_offsets", ["SILK_MAX_PULSES + 1"], None),
        ("silk_sign_iCDF", ["42"], None),  # NOT an iCDF — see emission comment
        ("silk_lsb_iCDF", ["2"], "rows"),
        # tables_LTP.c
        ("silk_LTP_per_index_iCDF", ["NB_LTP_CBKS"], "rows"),
        ("silk_LTP_gain_iCDF_0", ["8"], "rows"),
        ("silk_LTP_gain_iCDF_1", ["16"], "rows"),
        ("silk_LTP_gain_iCDF_2", ["32"], "rows"),
        ("silk_LTP_gain_vq_0", ["8", "LTP_ORDER"], None),
        ("silk_LTP_gain_vq_1", ["16", "LTP_ORDER"], None),
        ("silk_LTP_gain_vq_2", ["32", "LTP_ORDER"], None),
        ("silk_LTP_vq_sizes", ["NB_LTP_CBKS"], None),
        # tables_other.c
        ("silk_stereo_pred_quant_Q13", ["STEREO_QUANT_TAB_SIZE"], None),
        ("silk_stereo_pred_joint_iCDF", ["25"], "rows"),
        ("silk_stereo_only_code_mid_iCDF", ["2"], "rows"),
        ("silk_LBRR_flags_2_iCDF", ["3"], "rows"),
        ("silk_LBRR_flags_3_iCDF", ["7"], "rows"),
        ("silk_LTPscale_iCDF", ["3"], "rows"),
        ("silk_type_offset_VAD_iCDF", ["4"], "rows"),
        ("silk_type_offset_no_VAD_iCDF", ["2"], "rows"),
        ("silk_NLSF_interpolation_factor_iCDF", ["5"], "rows"),
        ("silk_Quantization_Offsets_Q10", ["2", "2"], None),
        ("silk_LTPScales_table_Q14", ["3"], None),
        ("silk_uniform3_iCDF", ["3"], "rows"),
        ("silk_uniform4_iCDF", ["4"], "rows"),
        ("silk_uniform5_iCDF", ["5"], "rows"),
        ("silk_uniform6_iCDF", ["6"], "rows"),
        ("silk_uniform8_iCDF", ["8"], "rows"),
        ("silk_NLSF_EXT_iCDF", ["7"], "rows"),
        ("silk_Transition_LP_B_Q28", ["TRANSITION_INT_NUM", "TRANSITION_NB"], None),
        ("silk_Transition_LP_A_Q28", ["TRANSITION_INT_NUM", "TRANSITION_NA"], None),
        # table_LSF_cos.c
        ("silk_LSFCosTab_FIX_Q12", ["LSF_COS_TAB_SZ_FIX + 1"], None),
        # NLSF codebooks (structs verified separately below)
        ("silk_NLSF_CB1_NB_MB_Q8", ["320"], None),
        ("silk_NLSF_CB1_Wght_Q9", ["320"], None),
        ("silk_NLSF_CB1_iCDF_NB_MB", ["64"], 32),
        ("silk_NLSF_CB2_SELECT_NB_MB", ["160"], None),
        ("silk_NLSF_CB2_iCDF_NB_MB", ["72"], 9),
        ("silk_NLSF_CB2_BITS_NB_MB_Q5", ["72"], None),
        ("silk_NLSF_PRED_NB_MB_Q8", ["18"], None),
        ("silk_NLSF_DELTA_MIN_NB_MB_Q15", ["11"], None),
        ("silk_NLSF_CB1_WB_Q8", ["512"], None),
        ("silk_NLSF_CB1_WB_Wght_Q9", ["512"], None),
        ("silk_NLSF_CB1_iCDF_WB", ["64"], 32),
        ("silk_NLSF_CB2_SELECT_WB", ["256"], None),
        ("silk_NLSF_CB2_iCDF_WB", ["72"], 9),
        ("silk_NLSF_CB2_BITS_WB_Q5", ["72"], None),
        ("silk_NLSF_PRED_WB_Q8", ["30"], None),
        ("silk_NLSF_DELTA_MIN_WB_Q15", ["17"], None),
        # resampler_rom.{h,c}
        ("silk_resampler_up2_hq_0", ["3"], None),
        ("silk_resampler_up2_hq_1", ["3"], None),
        ("silk_Resampler_3_4_COEFS",
         ["2 + 3*RESAMPLER_DOWN_ORDER_FIR0/2"], None),
        ("silk_Resampler_2_3_COEFS",
         ["2 + 2*RESAMPLER_DOWN_ORDER_FIR0/2"], None),
        ("silk_Resampler_1_2_COEFS", ["2 + RESAMPLER_DOWN_ORDER_FIR1/2"], None),
        ("silk_Resampler_1_3_COEFS", ["2 + RESAMPLER_DOWN_ORDER_FIR2/2"], None),
        ("silk_Resampler_1_4_COEFS", ["2 + RESAMPLER_DOWN_ORDER_FIR2/2"], None),
        ("silk_Resampler_1_6_COEFS", ["2 + RESAMPLER_DOWN_ORDER_FIR2/2"], None),
        ("silk_Resampler_2_3_COEFS_LQ", ["2 + 2*2"], None),
        ("silk_resampler_frac_FIR_12",
         ["12", "RESAMPLER_ORDER_FIR_12/2"], None),
    ]

    summary, repeats = [], []

    def note(name, tab, extra=""):
        shape = "x".join(str(d) for d in tab["dims"])
        summary.append(f"  {camel(name):32s} {TYPE_MAP[tab['ctype']][0]:8s} "
                       f"[{shape}]  {extra}".rstrip())

    # ---- verify every spec'd table -----------------------------------------
    for name, dim_exprs, icdf in spec:
        if name not in tables:
            fail(f"table {name} not found in any parsed source file")
        tab = tables[name]
        want = [E(d) for d in dim_exprs]
        if tab["dims"] != want:
            fail(f"{name}: source dims {tab['dims']} != expected {want}")
        check_range(name, tab)
        if name in externs:
            ext = externs[name]
            if ext["dims"] != tab["dims"] or ext["ctype"] != tab["ctype"]:
                fail(f"{name}: extern decl {ext} disagrees with definition "
                     f"{tab['ctype']} {tab['dims']}")
        extra = "extern-checked" if name in externs else ""
        if icdf == "rows":
            if tab["ctype"] != "opus_uint8":
                fail(f"{name}: iCDF table is not opus_uint8")
            if len(tab["dims"]) == 1:
                check_icdf_row(name, tab["vals"], repeats)
                n_rows = 1
            else:
                rows, cols = tab["dims"]
                for r in range(rows):
                    check_icdf_row(f"{name}[{r}]",
                                   tab["vals"][r * cols:(r + 1) * cols], repeats)
                n_rows = rows
            extra += f"  icdf ok ({n_rows} row{'s' if n_rows > 1 else ''})"
        elif isinstance(icdf, int):
            if len(tab["vals"]) % icdf != 0:
                fail(f"{name}: length {len(tab['vals'])} not a multiple of {icdf}")
            for r in range(len(tab["vals"]) // icdf):
                check_icdf_row(f"{name}[{r}]",
                               tab["vals"][r * icdf:(r + 1) * icdf], repeats)
            extra += f"  icdf ok ({len(tab['vals']) // icdf} rows of {icdf})"
        note(name, tab, extra.strip())

    # ---- shell-code tables: rows located by the offsets table --------------
    offs = tables["silk_shell_code_table_offsets"]["vals"]
    max_pulses = E("SILK_MAX_PULSES")
    if len(offs) != max_pulses + 1:
        fail(f"shell_code_table_offsets: {len(offs)} != SILK_MAX_PULSES+1")
    for k in range(1, max_pulses + 1):
        if offs[k] != offs[k - 1] + (k if k > 1 else 0):
            fail(f"shell_code_table_offsets[{k}] = {offs[k]}: rows are not "
                 f"contiguous iCDFs of length k+1")
    for t in range(4):
        name = f"silk_shell_code_table{t}"
        vals = tables[name]["vals"]
        if offs[max_pulses] + max_pulses + 1 != len(vals):
            fail(f"{name}: offsets table does not tile the array")
        for k in range(1, max_pulses + 1):
            check_icdf_row(f"{name}[k={k}]", vals[offs[k]:offs[k] + k + 1], repeats)
    summary.append("  shell code tables 0-3: 16 iCDF rows each (k=1..16, "
                   "length k+1), tiled per offsets table")

    # ---- silk_sign_iCDF: 6 blocks of 7 single Q8 probabilities -------------
    sign = tables["silk_sign_iCDF"]["vals"]
    if len(sign) != 6 * 7:
        fail("silk_sign_iCDF: expected 6 blocks of 7")
    for v in sign:
        if not (1 <= v <= 255):
            fail(f"silk_sign_iCDF: probability {v} out of (0, 255]")
    summary.append("  silk_sign_iCDF: exempt from iCDF check (6x7 single Q8 "
                   "probabilities, decoder builds 2-entry iCDFs)")

    # ---- LTP pointer tables and vq sizes ------------------------------------
    ltp_sizes = tables["silk_LTP_vq_sizes"]["vals"]
    if ltp_sizes != [8, 16, 32]:
        fail(f"silk_LTP_vq_sizes: {ltp_sizes} != [8, 16, 32]")
    for i, n in enumerate(ltp_sizes):
        if tables[f"silk_LTP_gain_iCDF_{i}"]["dims"] != [n]:
            fail(f"silk_LTP_gain_iCDF_{i}: length != vq_sizes[{i}] = {n}")
        if tables[f"silk_LTP_gain_vq_{i}"]["dims"] != [n, E("LTP_ORDER")]:
            fail(f"silk_LTP_gain_vq_{i}: dims != [{n}][LTP_ORDER]")
    ltp = src["tables_LTP.c"]
    icdf_ptrs = parse_ptr_table_names(ltp, "silk_LTP_gain_iCDF_ptrs", 3)
    vq_ptrs = parse_ptr_table_names(ltp, "silk_LTP_vq_ptrs_Q7", 3)
    if icdf_ptrs != [f"silk_LTP_gain_iCDF_{i}" for i in range(3)]:
        fail(f"silk_LTP_gain_iCDF_ptrs order unexpected: {icdf_ptrs}")
    if vq_ptrs != [f"silk_LTP_gain_vq_{i}" for i in range(3)]:
        fail(f"silk_LTP_vq_ptrs_Q7 order unexpected: {vq_ptrs}")
    lbrr_ptrs = parse_ptr_table_names(src["tables_other.c"],
                                      "silk_LBRR_flags_iCDF_ptr", 2)
    if lbrr_ptrs != ["silk_LBRR_flags_2_iCDF", "silk_LBRR_flags_3_iCDF"]:
        fail(f"silk_LBRR_flags_iCDF_ptr order unexpected: {lbrr_ptrs}")
    summary.append("  LTP/LBRR pointer tables: initializer order verified "
                   "against the source")

    # ---- resampler scalar coefficients --------------------------------------
    for s in ("silk_resampler_down2_0", "silk_resampler_down2_1"):
        if s not in scalars:
            fail(f"scalar {s} not found in resampler_rom.h")
        v = scalars[s]["val"]
        if not (-32768 <= v <= 32767):
            fail(f"{s}: {v} out of int16 range")
    summary.append(f"  resampler down2 coefs: "
                   f"{scalars['silk_resampler_down2_0']['val']}, "
                   f"{scalars['silk_resampler_down2_1']['val']}")

    # ---- NLSF codebook structs ----------------------------------------------
    struct_fields = ["nVectors", "order", "quantStepSize_Q16",
                     "invQuantStepSize_Q6", "CB1_NLSF_Q8", "CB1_Wght_Q9",
                     "CB1_iCDF", "pred_Q8", "ec_sel", "ec_iCDF", "ec_Rates_Q5",
                     "deltaMin_Q15"]
    m = re.search(r"typedef struct\s*\{([^}]*)\}\s*silk_NLSF_CB_struct\s*;",
                  src["structs.h"])
    if not m:
        fail("silk_NLSF_CB_struct definition not found in structs.h")
    found_fields = re.findall(r"(\w+)\s*;", m.group(1))
    if found_fields != struct_fields:
        fail(f"silk_NLSF_CB_struct fields changed: {found_fields}")

    def fix_const(expr):
        fm = re.fullmatch(r"SILK_FIX_CONST\s*\(\s*([^,]+),\s*(\d+)\s*\)",
                          expr.strip())
        if not fm:
            fail(f"expected SILK_FIX_CONST(...), got {expr!r}")
        c = eval(fm.group(1), {"__builtins__": {}}, {})  # float arithmetic
        return int(c * (1 << int(fm.group(2))) + 0.5)

    def parse_nlsf_struct(text, cname):
        sm = re.search(r"const\s+silk_NLSF_CB_struct\s+" + re.escape(cname) +
                       r"\s*=\s*\{(.*?)\};", text, re.S)
        if not sm:
            fail(f"{cname} struct initializer not found")
        entries, depth, cur = [], 0, []
        for c in sm.group(1):
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            if c == "," and depth == 0:
                entries.append("".join(cur).strip())
                cur = []
            else:
                cur.append(c)
        if "".join(cur).strip():
            entries.append("".join(cur).strip())
        entries = [e for e in entries if e]
        if len(entries) != 12:
            fail(f"{cname}: expected 12 initializer entries, got {len(entries)}")
        return {
            "nVectors": int(entries[0]),
            "order": int(entries[1]),
            "quantStepSize_Q16": fix_const(entries[2]),
            "invQuantStepSize_Q6": fix_const(entries[3]),
            "arrays": entries[4:],
        }

    nlsf_cbs = {}
    for cname, fn in (("silk_NLSF_CB_NB_MB", "tables_NLSF_CB_NB_MB.c"),
                      ("silk_NLSF_CB_WB", "tables_NLSF_CB_WB.c")):
        cb = parse_nlsf_struct(src[fn], cname)
        nv, order = cb["nVectors"], cb["order"]
        arrs = {fname: tables[aname] for fname, aname
                in zip(struct_fields[4:], cb["arrays"])}
        for fname, aname in zip(struct_fields[4:], cb["arrays"]):
            if aname not in tables:
                fail(f"{cname}: struct references unparsed array {aname}")
        shapes = {
            "CB1_NLSF_Q8": nv * order,
            "CB1_Wght_Q9": nv * order,
            "CB1_iCDF": 2 * nv,
            "pred_Q8": 2 * (order - 1),
            "ec_sel": nv * order // 2,
            "ec_iCDF": 8 * (2 * E("NLSF_QUANT_MAX_AMPLITUDE") + 1),
            "ec_Rates_Q5": 8 * (2 * E("NLSF_QUANT_MAX_AMPLITUDE") + 1),
            "deltaMin_Q15": order + 1,
        }
        for fname, wantlen in shapes.items():
            got = len(arrs[fname]["vals"])
            if got != wantlen:
                fail(f"{cname}.{fname}: length {got} != {wantlen}")
        # CB1 vectors should be non-decreasing NLSF vectors (report only).
        cb1 = arrs["CB1_NLSF_Q8"]["vals"]
        nondec = all(cb1[i * order + j] <= cb1[i * order + j + 1]
                     for i in range(nv) for j in range(order - 1))
        if not (-32768 <= cb["quantStepSize_Q16"] <= 32767 and
                -32768 <= cb["invQuantStepSize_Q6"] <= 32767):
            fail(f"{cname}: step sizes exceed int16")
        nlsf_cbs[cname] = cb
        summary.append(f"  {cname}: nVectors {nv}, order {order}, "
                       f"quantStepSize_Q16 {cb['quantStepSize_Q16']}, "
                       f"invQuantStepSize_Q6 {cb['invQuantStepSize_Q6']}, "
                       f"all 8 array shapes verified"
                       + ("" if nondec else "  [WARN: CB1 rows not sorted]"))

    total_bytes = sum(nbytes(tables[name]) for name, _, _ in spec) + 2 * 2
    print(f"all cross-checks passed: {len(spec)} tables + 2 scalars + "
          f"2 NLSF codebook structs, {total_bytes} table bytes")
    if repeats:
        print("non-strict iCDF rows (repeated values -> zero-probability "
              "symbols, matches source):")
        for name, n in repeats:
            print(f"  {name}: {n} repeat(s)")
    else:
        print("all iCDFs strictly decreasing")
    print("\n".join(summary))

    # ---- emit ----------------------------------------------------------------
    h = []
    h.append("// glint - Opus SILK decoder tables (GENERATED FILE — do not edit)")
    h.append("// Regenerate with: python tools/gen_opus_silk_tables.py")
    h.append("//")
    h.append("// Extracted from libopus 1.5.2 (BSD): silk/tables_*.c,")
    h.append("// silk/pitch_est_tables.c, silk/table_LSF_cos.c, silk/resampler_rom.{c,h}.")
    h.append("// The generator cross-checks parsed element counts against the dimensions")
    h.append("// declared at the definition site AND the extern declarations in")
    h.append("// silk/tables.h, validates every iCDF row (uint8, non-increasing, final 0),")
    h.append("// the shell-code row tiling, the LTP codebook sizes/pointer order, and the")
    h.append("// NLSF codebook shape invariants before emitting.")
    h.append("//")
    h.append("// iCDF convention (RFC 6716 §4.1.3.3): ec_dec_icdf with ft = 1<<8; entry i")
    h.append("// is 256 minus the cumulative frequency up to and including symbol i, so")
    h.append("// rows are non-increasing and end at 0. All SILK decode calls use ftb = 8.")
    h.append("//")
    h.append("// Encoder-only tables deliberately NOT emitted (verified referenced only")
    h.append("// from encoder sources): silk_pulses_per_block_BITS_Q5,")
    h.append("// silk_rate_levels_BITS_Q5, silk_LTP_gain_BITS_Q5_{0,1,2} (+ptr table),")
    h.append("// silk_LTP_gain_vq_{0,1,2}_gain / silk_LTP_vq_gain_ptrs_Q7,")
    h.append("// silk_Lag_range_stage3{,_10_ms}, silk_nb_cbk_searchs_stage3.")
    h.append("")
    h.append("#ifndef GLINT_OPUS_SILK_TABLES_HPP")
    h.append("#define GLINT_OPUS_SILK_TABLES_HPP")
    h.append("")
    h.append("#include <cstdint>")
    h.append("")
    h.append("namespace glint {")
    h.append("namespace opus {")
    h.append("namespace silk {")
    h.append("")
    h.append("// Structural constants (silk/define.h, pitch_est_defines.h,")
    h.append("// resampler_rom.h); the generator cross-checks them against the")
    h.append("// table shapes below.")
    consts = [
        ("kNLevelsQGain", "N_LEVELS_QGAIN", "gain quantization levels"),
        ("kNRateLevels", "N_RATE_LEVELS", "rate levels for pulse coding"),
        ("kMaxPulses", "SILK_MAX_PULSES", "max pulses per shell block"),
        ("kShellCodecFrameLength", "SHELL_CODEC_FRAME_LENGTH",
         "samples per shell block"),
        ("kNbLtpCbks", "NB_LTP_CBKS", "LTP codebooks (periodicity index)"),
        ("kLtpOrder", "LTP_ORDER", "LTP filter taps"),
        ("kMaxLpcOrder", "MAX_LPC_ORDER", "LPC order (WB; NB/MB use 10)"),
        ("kNlsfQuantMaxAmplitude", "NLSF_QUANT_MAX_AMPLITUDE",
         "NLSF residual iCDF half-width"),
        ("kPitchEstMinLagMs", "PITCH_EST_MIN_LAG_MS", "min pitch lag"),
        ("kPitchEstMaxLagMs", "PITCH_EST_MAX_LAG_MS", "max pitch lag"),
        ("kPeMaxNbSubfr", "PE_MAX_NB_SUBFR", "subframes per 20 ms frame"),
        ("kStereoQuantTabSize", "STEREO_QUANT_TAB_SIZE",
         "stereo predictor levels"),
        ("kTransitionIntNum", "TRANSITION_INT_NUM", "transition LP interp points"),
        ("kTransitionNb", "TRANSITION_NB", "transition LP FIR taps"),
        ("kTransitionNa", "TRANSITION_NA", "transition LP IIR taps"),
        ("kLsfCosTabSize", "LSF_COS_TAB_SZ_FIX", "LSF cosine table size"),
        ("kResamplerDownOrderFir0", "RESAMPLER_DOWN_ORDER_FIR0", ""),
        ("kResamplerDownOrderFir1", "RESAMPLER_DOWN_ORDER_FIR1", ""),
        ("kResamplerDownOrderFir2", "RESAMPLER_DOWN_ORDER_FIR2", ""),
        ("kResamplerOrderFir12", "RESAMPLER_ORDER_FIR_12", ""),
    ]
    for kname, mname, desc in consts:
        v = E(mname)
        h.append(f"constexpr int {kname} = {v};"
                 + (f"  // {desc}" if desc else ""))
    h.append("")

    def emit(name, *comment):
        emit_array(h, tables[name], name, comment)
        h.append("")

    h.append("// ---- gains (silk/tables_gain.c) ------------------------------------------")
    h.append("")
    emit("silk_gain_iCDF",
         "Independent (MSB) gain index iCDFs, [signalType] (0 inactive,",
         "1 unvoiced, 2 voiced); the 3 LSBs are coded with kUniform8Icdf.")
    emit("silk_delta_gain_iCDF",
         "Delta gain index iCDF (MIN_DELTA_GAIN_QUANT -4 .. MAX 36).")
    h.append("// ---- pitch lag / contour (silk/tables_pitch_lag.c, pitch_est_tables.c) ---")
    h.append("")
    emit("silk_pitch_lag_iCDF",
         "High part of the absolute pitch lag, 32 symbols of fs_kHz/2 lags each.")
    emit("silk_pitch_delta_iCDF",
         "Relative-lag delta iCDF; symbol 0 escapes to absolute coding.")
    emit("silk_pitch_contour_iCDF", "Pitch contour index, 20 ms MB/WB (34 entries).")
    emit("silk_pitch_contour_NB_iCDF", "Pitch contour index, 20 ms NB.")
    emit("silk_pitch_contour_10_ms_iCDF", "Pitch contour index, 10 ms MB/WB.")
    emit("silk_pitch_contour_10_ms_NB_iCDF", "Pitch contour index, 10 ms NB.")
    emit("silk_CB_lags_stage2",
         "Per-subframe lag offsets for the 20 ms contour codebook, stage 2",
         "(decode_pitch.c uses the first PE_NB_CBKS_STAGE2 = 3 columns for NB).")
    emit("silk_CB_lags_stage3",
         "Per-subframe lag offsets, 20 ms stage 3 (MB/WB, 34 contours).")
    emit("silk_CB_lags_stage2_10_ms", "Lag offsets, 10 ms stage 2.")
    emit("silk_CB_lags_stage3_10_ms", "Lag offsets, 10 ms stage 3.")
    h.append("// ---- excitation: rate levels, pulse counts, shell coder, LSBs, signs ------")
    h.append("// (silk/tables_pulses_per_block.c)")
    h.append("")
    emit("silk_max_pulses_table",
         "Max pulses per shell block for rate levels (encoder-side cap; kept",
         "for the future O5 encoder — tiny).")
    emit("silk_pulses_per_block_iCDF",
         "Pulse-count iCDF per rate level; rows 9 (with LSB escape at symbol",
         "17) and 10 are the two escape distributions.")
    emit("silk_rate_levels_iCDF",
         "Rate level iCDF, [signalType >> 1] (unvoiced/voiced).")
    for t in range(4):
        emit(f"silk_shell_code_table{t}",
             f"Shell-coder split iCDFs, table {t}: row for k pulses (k=1..16)",
             "has k+1 entries and starts at kShellCodeTableOffsets[k].")
    emit("silk_shell_code_table_offsets", "Row offsets into the shell code tables.")
    emit("silk_lsb_iCDF", "Excitation LSB iCDF (used when nLshifts > 0).")
    emit("silk_sign_iCDF",
         "NOT an iCDF: 6 blocks of 7 single Q8 zero-probabilities indexed",
         "7*(quantOffsetType + (signalType << 1)) + min(sum_pulses, 6); the",
         "decoder builds the 2-entry iCDF { p, 0 } per shell block (code_signs.c).")
    h.append("// ---- LTP (silk/tables_LTP.c) ----------------------------------------------")
    h.append("")
    emit("silk_LTP_per_index_iCDF", "Periodicity index (selects the LTP codebook).")
    emit("silk_LTP_gain_iCDF_0", "LTP gain index iCDF, codebook 0 (8 vectors).")
    emit("silk_LTP_gain_iCDF_1", "LTP gain index iCDF, codebook 1 (16 vectors).")
    emit("silk_LTP_gain_iCDF_2", "LTP gain index iCDF, codebook 2 (32 vectors).")
    emit("silk_LTP_gain_vq_0", "LTP filter codebook 0, Q7, kLtpOrder taps per row.")
    emit("silk_LTP_gain_vq_1", "LTP filter codebook 1, Q7.")
    emit("silk_LTP_gain_vq_2", "LTP filter codebook 2, Q7.")
    emit("silk_LTP_vq_sizes", "Codebook sizes {8, 16, 32}.")
    h.append("// Pointer tables mirroring silk_LTP_gain_iCDF_ptrs / silk_LTP_vq_ptrs_Q7")
    h.append("// (initializer order verified against the source).")
    h.append("inline constexpr const uint8_t* kLtpGainIcdfPtrs[kNbLtpCbks] = {")
    h.append("    kLtpGainIcdf0, kLtpGainIcdf1, kLtpGainIcdf2,")
    h.append("};")
    h.append("inline constexpr const int8_t* kLtpVqPtrsQ7[kNbLtpCbks] = {")
    h.append("    &kLtpGainVq0[0][0], &kLtpGainVq1[0][0], &kLtpGainVq2[0][0],")
    h.append("};")
    h.append("")
    emit("silk_LTPscale_iCDF", "LTP scaling parameter iCDF (silk/tables_other.c).")
    emit("silk_LTPScales_table_Q14", "LTP scale values, Q14.")
    h.append("// ---- NLSF codebooks (silk/tables_NLSF_CB_NB_MB.c, tables_NLSF_CB_WB.c) ---")
    h.append("//")
    h.append("// Layout mirrors silk_NLSF_CB_struct: CB1 stage-1 vectors (nVectors x")
    h.append("// order, Q8) with per-coefficient weights (Q9); CB1_iCDF holds TWO iCDFs")
    h.append("// of nVectors entries, selected by signalType >> 1; ec_sel packs two")
    h.append("// 4-bit entries per byte -> per-coefficient residual iCDF row (of length")
    h.append("// 2*kNlsfQuantMaxAmplitude + 1 within ec_iCDF) and predictor choice from")
    h.append("// pred_Q8; deltaMin_Q15 (order+1) feeds NLSF stabilization. ec_Rates_Q5")
    h.append("// is encoder-only but kept to mirror the struct (72 B per codebook).")
    h.append("")
    emit("silk_NLSF_CB1_NB_MB_Q8", "NB/MB stage-1 codebook, 32 vectors x order 10.")
    emit("silk_NLSF_CB1_Wght_Q9", "NB/MB stage-1 weights.")
    emit("silk_NLSF_CB1_iCDF_NB_MB", "NB/MB stage-1 index iCDFs (2 x 32).")
    emit("silk_NLSF_CB2_SELECT_NB_MB", "NB/MB ec_sel, 32 vectors x order/2 bytes.")
    emit("silk_NLSF_CB2_iCDF_NB_MB", "NB/MB residual iCDFs, 8 rows of 9.")
    emit("silk_NLSF_CB2_BITS_NB_MB_Q5", "NB/MB residual rates, Q5 (encoder-only).")
    emit("silk_NLSF_PRED_NB_MB_Q8", "NB/MB backwards predictors, 2 x (order-1).")
    emit("silk_NLSF_DELTA_MIN_NB_MB_Q15", "NB/MB minimum NLSF spacing, order+1.")
    emit("silk_NLSF_CB1_WB_Q8", "WB stage-1 codebook, 32 vectors x order 16.")
    emit("silk_NLSF_CB1_WB_Wght_Q9", "WB stage-1 weights.")
    emit("silk_NLSF_CB1_iCDF_WB", "WB stage-1 index iCDFs (2 x 32).")
    emit("silk_NLSF_CB2_SELECT_WB", "WB ec_sel, 32 vectors x order/2 bytes.")
    emit("silk_NLSF_CB2_iCDF_WB", "WB residual iCDFs, 8 rows of 9.")
    emit("silk_NLSF_CB2_BITS_WB_Q5", "WB residual rates, Q5 (encoder-only).")
    emit("silk_NLSF_PRED_WB_Q8", "WB backwards predictors, 2 x (order-1).")
    emit("silk_NLSF_DELTA_MIN_WB_Q15", "WB minimum NLSF spacing, order+1.")
    h.append("// Mirror of silk_NLSF_CB_struct (silk/structs.h; field order verified).")
    h.append("struct NlsfCodebook {")
    h.append("    int16_t nVectors;")
    h.append("    int16_t order;")
    h.append("    int16_t quantStepSizeQ16;")
    h.append("    int16_t invQuantStepSizeQ6;")
    h.append("    const uint8_t* cb1NlsfQ8;")
    h.append("    const int16_t* cb1WghtQ9;")
    h.append("    const uint8_t* cb1Icdf;")
    h.append("    const uint8_t* predQ8;")
    h.append("    const uint8_t* ecSel;")
    h.append("    const uint8_t* ecIcdf;")
    h.append("    const uint8_t* ecRatesQ5;")
    h.append("    const int16_t* deltaMinQ15;")
    h.append("};")
    for cname, kname in (("silk_NLSF_CB_NB_MB", "kNlsfCbNbMb"),
                         ("silk_NLSF_CB_WB", "kNlsfCbWb")):
        cb = nlsf_cbs[cname]
        arr_names = [camel(a) for a in cb["arrays"]]
        h.append(f"inline constexpr NlsfCodebook {kname} = {{")
        h.append(f"    {cb['nVectors']}, {cb['order']}, "
                 f"{cb['quantStepSize_Q16']}, {cb['invQuantStepSize_Q6']},")
        h.append(f"    {arr_names[0]}, {arr_names[1]}, {arr_names[2]}, "
                 f"{arr_names[3]},")
        h.append(f"    {arr_names[4]}, {arr_names[5]}, {arr_names[6]}, "
                 f"{arr_names[7]},")
        h.append("};")
    h.append("")
    emit("silk_NLSF_interpolation_factor_iCDF",
         "NLSF interpolation factor for 20 ms frames (silk/tables_other.c).")
    emit("silk_NLSF_EXT_iCDF", "NLSF residual extension (values beyond +-4).")
    emit("silk_LSFCosTab_FIX_Q12",
         "Piecewise-linear cosine table for NLSF -> LPC (silk/table_LSF_cos.c),",
         "Q12, kLsfCosTabSize + 1 entries over [0, pi].")
    h.append("// ---- stereo, frame type, misc (silk/tables_other.c) -----------------------")
    h.append("")
    emit("silk_stereo_pred_quant_Q13", "Stereo predictor quantization levels.")
    emit("silk_stereo_pred_joint_iCDF", "Joint iCDF for the two predictor MSB indices.")
    emit("silk_stereo_only_code_mid_iCDF", "P(side channel not coded).")
    emit("silk_LBRR_flags_2_iCDF", "LBRR flags, 2 frames per packet (40 ms).")
    emit("silk_LBRR_flags_3_iCDF", "LBRR flags, 3 frames per packet (60 ms).")
    h.append("// Mirror of silk_LBRR_flags_iCDF_ptr, indexed nFramesPerPacket - 2.")
    h.append("inline constexpr const uint8_t* kLbrrFlagsIcdfPtr[2] = {")
    h.append("    kLbrrFlags2Icdf, kLbrrFlags3Icdf,")
    h.append("};")
    h.append("")
    emit("silk_type_offset_VAD_iCDF",
         "Frame type when VAD active: (signalType - 1) * 2 + quantOffsetType.")
    emit("silk_type_offset_no_VAD_iCDF", "Frame type when VAD inactive.")
    emit("silk_Quantization_Offsets_Q10",
         "Excitation offsets, [signalType >> 1][quantOffsetType], Q10.")
    emit("silk_uniform3_iCDF",
         "Uniform iCDFs (stereo predictor LSBs, pitch lag LSBs, LCG seed, ...).")
    emit("silk_uniform4_iCDF", "")
    emit("silk_uniform5_iCDF", "")
    emit("silk_uniform6_iCDF", "")
    emit("silk_uniform8_iCDF", "")
    emit("silk_Transition_LP_B_Q28",
         "Bandwidth-switching transition LP filter, FIR interpolation points",
         "(silk/LP_variable_cutoff.c), Q28.")
    emit("silk_Transition_LP_A_Q28", "Transition LP filter, IIR interpolation points.")
    h.append("// ---- resampler ROM (silk/resampler_rom.{h,c}) ------------------------------")
    h.append("// COEFS layout: 2 IIR coefs then the FIR half-phases (order/2 per phase).")
    h.append("")
    h.append("// Allpass coefficients for the 2x downsampler, Q16 (resampler_down2).")
    h.append(f"inline constexpr int16_t kResamplerDown2Coef0 = "
             f"{scalars['silk_resampler_down2_0']['val']};")
    h.append(f"inline constexpr int16_t kResamplerDown2Coef1 = "
             f"{scalars['silk_resampler_down2_1']['val']};")
    h.append("")
    emit("silk_resampler_up2_hq_0",
         "Allpass coefficients for the high-quality 2x upsampler, Q16.")
    emit("silk_resampler_up2_hq_1", "")
    emit("silk_Resampler_3_4_COEFS", "Fractional downsampler 3/4 (e.g. 16 -> 12 kHz).")
    emit("silk_Resampler_2_3_COEFS", "Fractional downsampler 2/3.")
    emit("silk_Resampler_1_2_COEFS", "Downsampler 1/2.")
    emit("silk_Resampler_1_3_COEFS", "Downsampler 1/3.")
    emit("silk_Resampler_1_4_COEFS", "Downsampler 1/4.")
    emit("silk_Resampler_1_6_COEFS", "Downsampler 1/6.")
    emit("silk_Resampler_2_3_COEFS_LQ", "Low-quality 2/3 downsampler (down2_3).")
    emit("silk_resampler_frac_FIR_12",
         "Interpolating FIR half-phases for fractions 1/24, 3/24, ..., 23/24",
         "(resampler_private_IIR_FIR upsampling).")
    h.append("}  // namespace silk")
    h.append("}  // namespace opus")
    h.append("}  // namespace glint")
    h.append("")
    h.append("#endif  // GLINT_OPUS_SILK_TABLES_HPP")

    with open(args.out, "w") as f:
        f.write("\n".join(h) + "\n")
    print(f"wrote {args.out} ({os.path.getsize(args.out)} bytes)")


if __name__ == "__main__":
    main()
