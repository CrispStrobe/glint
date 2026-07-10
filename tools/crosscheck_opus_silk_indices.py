#!/usr/bin/env python3
"""Cross-check glint's SILK side-info decoding (decode_indices +
gains_dequant + decode_pitch) against libopus — byte-identical fuzz oracle
with per-sequence state chaining (conditional coding, prev lag/type).

Usage: python3 tools/crosscheck_opus_silk_indices.py
"""

import os
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
CUSTOM_SRC = os.path.join(TOOLS_DIR, "opus-1.5.2-custom")
CUSTOM_LIB = os.path.join(CUSTOM_SRC, ".libs", "libopus.a")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(REPO, "tools", "opus_silk_indices_crosscheck.cpp")


def run(cmd):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def main():
    if not os.path.exists(CUSTOM_LIB):
        sys.exit("missing custom-modes libopus; run "
                 "tools/crosscheck_opus_celt_prims.py once to build it")
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        ref_bin = os.path.join(tmp, "si_ref")
        glint_bin = os.path.join(tmp, "si_glint")
        run([cxx, "-std=c++17", "-O2", "-DUSE_LIBOPUS", "-DOPUS_BUILD",
             "-DCUSTOM_MODES", "-DUSE_ALLOCA",
             "-I", os.path.join(CUSTOM_SRC, "silk"),
             "-I", os.path.join(CUSTOM_SRC, "celt"),
             "-I", os.path.join(CUSTOM_SRC, "include"),
             DRIVER, CUSTOM_LIB, "-o", ref_bin])
        run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
             DRIVER, os.path.join(REPO, "src", "opus_ec.cpp"),
             os.path.join(REPO, "src", "opus_silk_indices.cpp"),
             "-o", glint_bin])
        ref = subprocess.run([ref_bin], check=True,
                             capture_output=True).stdout
        gl = subprocess.run([glint_bin], check=True,
                            capture_output=True).stdout
    if ref == gl:
        print(f"PASS: SILK side-info decode byte-identical with libopus "
              f"({ref.count(b'seed')} fuzzed sequences x 3 chained frames)")
        return 0
    for i, (a, b) in enumerate(zip(ref.splitlines(), gl.splitlines()), 1):
        if a != b:
            print(f"FAIL line {i}:\n  ref:   {a.decode()[:150]}\n"
                  f"  glint: {b.decode()[:150]}")
            return 1
    print("FAIL: length mismatch")
    return 1


if __name__ == "__main__":
    sys.exit(main())
