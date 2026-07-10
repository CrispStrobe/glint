#!/usr/bin/env python3
"""RFC 6716/8251 conformance: decode the official Opus test vectors with
glint's clean-room decoder and validate with opus_compare — the normative
conformance procedure (stereo and mono downmix, 48 kHz).

Usage: python3 tools/test_opus_vectors.py
"""

import os
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
VEC_DIR = os.path.join(TOOLS_DIR, "opus_newvectors")
OPUS_COMPARE = os.path.join(TOOLS_DIR, "opus_compare")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRCS = ["opus_ec.cpp", "opus_laplace.cpp", "opus_cwrs.cpp",
        "opus_celt_energy.cpp", "opus_celt_rate.cpp", "opus_celt_bands.cpp",
        "opus_mdct.cpp", "opus_celt_decoder.cpp", "opus_decoder.cpp",
        "opus_silk_excitation.cpp", "opus_silk_indices.cpp",
        "opus_silk_nlsf.cpp", "opus_silk_frame.cpp", "opus_silk_stereo.cpp",
        "opus_silk_resampler.cpp", "opus_silk_decoder.cpp"]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


def main():
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        cli = os.path.join(tmp, "opus_dec_cli")
        r = run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
                 os.path.join(REPO, "tools", "opus_dec_cli.cpp")] +
                [os.path.join(REPO, "src", s) for s in SRCS] + ["-o", cli])
        if r.returncode:
            sys.exit("CLI build failed:\n" + r.stderr.decode()[:800])

        passed = failed = 0
        for v in range(1, 13):
            bit = os.path.join(VEC_DIR, f"testvector{v:02d}.bit")
            ref = os.path.join(VEC_DIR, f"testvector{v:02d}.dec")
            mine = os.path.join(tmp, "mine.raw")
            r = run([cli, bit, "2", mine])
            if r.returncode:
                print(f"vector {v:02d}: DECODE FAIL "
                      f"({r.stderr.decode().strip().splitlines()[:1]})")
                failed += 1
                continue
            ranges = r.stdout.decode().strip()
            c = run([OPUS_COMPARE, "-s", "-r", "48000", ref, mine])
            out = (c.stdout.decode() + c.stderr.decode()).strip()
            quality = [l for l in out.splitlines() if l][-1] if out else "?"
            status = "PASS" if c.returncode == 0 else "FAIL"
            if c.returncode == 0:
                passed += 1
            else:
                failed += 1
            print(f"vector {v:02d}: {status} [{ranges}] {quality}")
        print(f"\n{passed}/12 official test vectors pass")
        return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
