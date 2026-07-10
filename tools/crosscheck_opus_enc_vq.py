#!/usr/bin/env python3
"""Cross-check glint's CELT encoder VQ layer (alg_quant / op_pvq_search /
stereo_itheta / exp_rotation / stereo helpers) against reference libopus.

Compiles tools/opus_enc_vq_crosscheck.cpp twice — once against the
custom-modes libopus float build (reference), once against
src/opus_celt_enc_vq.cpp + src/opus_{ec,cwrs}.cpp (glint) — and fuzzes 800
alg_quant cases over the real CELT domain (band sizes, k with V(n,k) < 2^32,
spread, B, gain) plus 400 stereo_itheta and 200 rotation cases. Integer
lines (encoded byte streams, tells, collapse masks, decoded pulse vectors,
itheta, round-trip flags) must match byte-for-byte — the float32 rules in
opus_celt_enc_vq.cpp make the pulse search decisions identical, so the
encoded streams must be identical. Lines starting with 'X' are float
spectra compared with 1e-6 tolerance. Wire-compatibility gate for
PLAN § O4 step 4.

Usage: python3 tools/crosscheck_opus_enc_vq.py
"""

import os
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
CUSTOM_SRC = os.path.join(TOOLS_DIR, "opus-1.5.2-custom")
CUSTOM_LIB = os.path.join(CUSTOM_SRC, ".libs", "libopus.a")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(REPO, "tools", "opus_enc_vq_crosscheck.cpp")
TOL = 1e-6  # float-vs-float residual normalization noise only


def run(cmd):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def main():
    if not os.path.exists(CUSTOM_LIB):
        sys.exit("missing custom-modes libopus; run "
                 "tools/crosscheck_opus_celt_prims.py once to build it")
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        ref_bin = os.path.join(tmp, "encvq_ref")
        glint_bin = os.path.join(tmp, "encvq_glint")
        run([cxx, "-std=c++17", "-O2", "-DUSE_LIBOPUS", "-DOPUS_BUILD",
             "-DCUSTOM_MODES", "-DUSE_ALLOCA",
             "-I", os.path.join(CUSTOM_SRC, "celt"),
             "-I", os.path.join(CUSTOM_SRC, "include"),
             DRIVER, CUSTOM_LIB, "-o", ref_bin])
        run([cxx, "-std=c++17", "-O2", "-Wall",
             "-I", os.path.join(REPO, "src"),
             DRIVER,
             os.path.join(REPO, "src", "opus_ec.cpp"),
             os.path.join(REPO, "src", "opus_cwrs.cpp"),
             os.path.join(REPO, "src", "opus_celt_enc_vq.cpp"),
             "-o", glint_bin])
        ref = subprocess.run([ref_bin], check=True,
                             capture_output=True).stdout.decode()
        gl = subprocess.run([glint_bin], check=True,
                            capture_output=True).stdout.decode()

    ref_lines = ref.splitlines()
    gl_lines = gl.splitlines()
    if len(ref_lines) != len(gl_lines):
        sys.exit(f"FAIL: line counts differ ({len(ref_lines)} vs "
                 f"{len(gl_lines)})")
    max_delta = 0.0
    n_rt = 0
    for ln, (a, b) in enumerate(zip(ref_lines, gl_lines), 1):
        if a.startswith("rt "):
            if a != b or not a.endswith(" 1"):
                sys.exit(f"FAIL (round-trip) line {ln}:\n  ref:   {a}\n"
                         f"  glint: {b}")
            n_rt += 1
            continue
        if not a.startswith("X"):
            if a != b:
                sys.exit(f"FAIL line {ln}:\n  ref:   {a[:150]}\n"
                         f"  glint: {b[:150]}")
            continue
        va, vb = a.split()[2:], b.split()[2:]
        if a.split()[:2] != b.split()[:2] or len(va) != len(vb):
            sys.exit(f"FAIL line {ln}: X-line headers/lengths differ")
        for sa, sb in zip(va, vb):
            d = abs(float(sa) - float(sb))
            max_delta = max(max_delta, d)
            if d > TOL:
                sys.exit(f"FAIL (spectrum) line {ln}: {sa} vs {sb}")
    print(f"PASS: encoder VQ layer byte-identical with libopus "
          f"(800 alg_quant cases: streams/tells/masks/pulse vectors exact, "
          f"{n_rt} round-trips exact, 400 itheta + 200 rotations + 100 "
          f"stereo-helper cases, max spectrum delta {max_delta:.2e})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
