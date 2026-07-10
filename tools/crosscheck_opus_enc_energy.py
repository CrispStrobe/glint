#!/usr/bin/env python3
"""Cross-check glint's CELT ENCODER-side energy pipeline against libopus.

Both sides run tools/opus_enc_energy_crosscheck.cpp over 300 fuzzed
scenarios (band analysis -> coarse two-pass -> fine -> finalise, with
oldEBands/delayedIntra chained across frames) and the entire stdout must be
BYTE-IDENTICAL: %a hex floats (bandE/bandLogE/error/oldEBands/delayedIntra),
tells at every stage, the encoded byte streams, and a self-decode
consistency verdict (each side decodes its stream with its own decoder —
glint's opus_celt_energy on the glint side).

Float-exactness strategy: the prebuilt libopus.a fuses FMAs / vectorizes
(legal but not source-reproducible), so the reference side compiles
celt/quant_bands.c and celt/bands.c FRESH with -ffp-contract=off, linked
against libopus.a for the range coder / laplace / mode machinery; the
glint side is compiled with the same pinned flag. IEEE semantics then
fully determine every float, making byte comparison meaningful.

Usage: python3 tools/crosscheck_opus_enc_energy.py
"""

import os
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
CUSTOM_SRC = os.path.join(TOOLS_DIR, "opus-1.5.2-custom")
CUSTOM_LIB = os.path.join(CUSTOM_SRC, ".libs", "libopus.a")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(REPO, "tools", "opus_enc_energy_crosscheck.cpp")

REF_DEFS = ["-DOPUS_BUILD", "-DCUSTOM_MODES", "-DUSE_ALLOCA"]
REF_INCS = ["-I", os.path.join(CUSTOM_SRC, "celt"),
            "-I", os.path.join(CUSTOM_SRC, "include")]
FP_FLAGS = ["-O2", "-ffp-contract=off"]


def run(cmd):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def main():
    if not os.path.exists(CUSTOM_LIB):
        sys.exit("missing custom-modes libopus; run "
                 "tools/crosscheck_opus_celt_prims.py once to build it")
    cxx = os.environ.get("CXX", "c++")
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        # Reference TUs with pinned IEEE float semantics.
        qb_o = os.path.join(tmp, "quant_bands.o")
        bands_o = os.path.join(tmp, "bands.o")
        run([cc, *FP_FLAGS, *REF_DEFS, *REF_INCS, "-c",
             os.path.join(CUSTOM_SRC, "celt", "quant_bands.c"), "-o", qb_o])
        run([cc, *FP_FLAGS, *REF_DEFS, *REF_INCS, "-c",
             os.path.join(CUSTOM_SRC, "celt", "bands.c"), "-o", bands_o])

        ref_bin = os.path.join(tmp, "enc_energy_ref")
        glint_bin = os.path.join(tmp, "enc_energy_glint")
        # Object files come before the archive, so the linker resolves
        # quant_bands/bands symbols from the pinned TUs and only pulls the
        # remaining machinery (ec, laplace, modes, ...) from libopus.a.
        run([cxx, "-std=c++17", *FP_FLAGS, "-DUSE_LIBOPUS", *REF_DEFS,
             *REF_INCS, DRIVER, qb_o, bands_o, CUSTOM_LIB, "-o", ref_bin])
        run([cxx, "-std=c++17", *FP_FLAGS, "-Wall",
             "-I", os.path.join(REPO, "src"), DRIVER,
             os.path.join(REPO, "src", "opus_ec.cpp"),
             os.path.join(REPO, "src", "opus_laplace.cpp"),
             os.path.join(REPO, "src", "opus_celt_energy.cpp"),
             os.path.join(REPO, "src", "opus_celt_enc_energy.cpp"),
             "-o", glint_bin])

        ref = subprocess.run([ref_bin], check=True,
                             capture_output=True).stdout.decode()
        gl = subprocess.run([glint_bin], check=True,
                            capture_output=True).stdout.decode()

    ref_lines = ref.splitlines()
    gl_lines = gl.splitlines()
    n = min(len(ref_lines), len(gl_lines))
    for ln in range(n):
        if ref_lines[ln] != gl_lines[ln]:
            sys.exit(f"FAIL line {ln + 1}:\n  ref:   "
                     f"{ref_lines[ln][:160]}\n  glint: "
                     f"{gl_lines[ln][:160]}")
    if len(ref_lines) != len(gl_lines):
        sys.exit(f"FAIL: line counts differ ({len(ref_lines)} vs "
                 f"{len(gl_lines)})")
    fails = sum("FAIL" in l for l in ref_lines)
    if fails:
        sys.exit(f"FAIL: {fails} self-decode mismatches")
    scen = sum(l.startswith("seed ") for l in ref_lines)
    frames = sum(l.startswith("frame ") for l in ref_lines)
    print(f"PASS: encoder energy pipeline byte-identical to libopus over "
          f"{scen} scenarios / {frames} frames (bytes, tells, %a floats, "
          f"self-decode consistent)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
