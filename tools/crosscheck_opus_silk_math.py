#!/usr/bin/env python3
"""Cross-check glint's SILK fixed-point kit against the reference macros.

SILK is exact integer arithmetic: 2M randomized operand sets through every
macro/function, hashes must be identical. On mismatch, reruns both in
verbose mode and reports the first differing value.

Usage: python3 tools/crosscheck_opus_silk_math.py
"""

import os
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
CUSTOM_SRC = os.path.join(TOOLS_DIR, "opus-1.5.2-custom")
CUSTOM_LIB = os.path.join(CUSTOM_SRC, ".libs", "libopus.a")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DRIVER = os.path.join(REPO, "tools", "opus_silk_math_crosscheck.cpp")


def run(cmd):
    print("+ " + " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def main():
    if not os.path.exists(CUSTOM_LIB):
        sys.exit("missing custom-modes libopus; run "
                 "tools/crosscheck_opus_celt_prims.py once to build it")
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        ref_bin = os.path.join(tmp, "sm_ref")
        glint_bin = os.path.join(tmp, "sm_glint")
        run([cxx, "-std=c++17", "-O2", "-DUSE_LIBOPUS", "-DOPUS_BUILD",
             "-DCUSTOM_MODES", "-DUSE_ALLOCA", "-DFIXED_POINT=0",
             "-I", os.path.join(CUSTOM_SRC, "silk"),
             "-I", os.path.join(CUSTOM_SRC, "celt"),
             "-I", os.path.join(CUSTOM_SRC, "include"),
             DRIVER, CUSTOM_LIB, "-o", ref_bin])
        run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
             DRIVER, "-o", glint_bin])
        ref = subprocess.run([ref_bin], check=True,
                             capture_output=True).stdout
        gl = subprocess.run([glint_bin], check=True,
                            capture_output=True).stdout
        if ref == gl:
            print(f"PASS: SILK fixed-point kit byte-identical "
                  f"({ref.decode().strip()}, 2M operand sets)")
            return 0
        print(f"HASH MISMATCH (ref {ref.decode().strip()} vs glint "
              f"{gl.decode().strip()}); locating in verbose mode...")
        ref_v = subprocess.run([ref_bin, "v"], check=True,
                               capture_output=True).stdout.splitlines()
        gl_v = subprocess.run([glint_bin, "v"], check=True,
                              capture_output=True).stdout.splitlines()
        for i, (a, b) in enumerate(zip(ref_v, gl_v)):
            if a != b:
                print(f"FAIL at value #{i}: ref {a.decode()} vs glint "
                      f"{b.decode()}")
                return 1
        print("FAIL: differs only beyond the verbose window (2000 iters)")
        return 1


if __name__ == "__main__":
    sys.exit(main())
