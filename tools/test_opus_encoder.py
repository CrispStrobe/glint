#!/usr/bin/env python3
"""Correctness gate for glint's CELT-only Opus ENCODER (the merge gate):

1. libopus (opus_demo -d) decodes every glint stream and — critically —
   verifies our stored final ranges against ITS decoder's range state:
   the Opus conformance identity, checked by the reference itself.
2. glint's own decoder produces PCM matching libopus's within a few LSB.
3. Decoded audio has a sane SNR floor vs the source (quality iterates
   later; this catches gross energy/shape bugs, not tuning).

Usage: python3 tools/test_opus_encoder.py
"""

import math
import os
import struct
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
OPUS_DEMO = os.path.join(TOOLS_DIR, "opus_demo")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEC_SRCS = ["opus_ec.cpp", "opus_laplace.cpp", "opus_cwrs.cpp",
            "opus_celt_energy.cpp", "opus_celt_rate.cpp",
            "opus_celt_bands.cpp", "opus_mdct.cpp", "opus_celt_decoder.cpp",
            "opus_decoder.cpp", "opus_silk_excitation.cpp",
            "opus_silk_indices.cpp", "opus_silk_nlsf.cpp",
            "opus_silk_plc.cpp", "opus_silk_frame.cpp",
            "opus_silk_stereo.cpp", "opus_silk_resampler.cpp",
            "opus_silk_decoder.cpp"]
ENC_SRCS = ["opus_ec.cpp", "opus_laplace.cpp", "opus_cwrs.cpp",
            "opus_celt_rate.cpp", "opus_mdct.cpp",
            "opus_celt_enc_energy.cpp", "opus_celt_enc_vq.cpp",
            "opus_celt_enc_bands.cpp", "opus_celt_encoder.cpp",
            "opus_celt_bands.cpp", "opus_celt_energy.cpp",
            "opus_celt_pitch.cpp"]


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


def gen_signal(path, channels, seconds=2):
    n = 48000 * seconds
    rng = 4242
    data = bytearray()
    for i in range(n):
        t = i / 48000.0
        s = 0.35 * math.sin(2 * math.pi * (150 + 120 * t) * t)
        s += 0.25 * math.sin(2 * math.pi * 1777 * t)
        s += 0.1 * math.sin(2 * math.pi * 7333 * t)
        rng = (1664525 * rng + 1013904223) & 0xFFFFFFFF
        s += (rng / 2**31 - 1.0) * 0.03
        for c in range(channels):
            v = max(-0.9, min(0.9, s * (1.0 if c == 0 else 0.7)))
            data += struct.pack("<h", int(v * 32767))
    open(path, "wb").write(data)


def snr(ref, test, channels, skip=960):
    # The CELT chain delays output by the MDCT overlap (120 samples/ch);
    # align before comparing.
    delay = 120 * channels
    n = min(len(ref) // 2 - delay, len(test) // 2 - delay)
    sig = err = 1e-30
    for i in range(skip, n):
        a = struct.unpack_from("<h", ref, 2 * i)[0]
        b = struct.unpack_from("<h", test, 2 * (i + delay))[0]
        sig += a * a
        err += (a - b) * (a - b)
    return 10 * math.log10(sig / err)


def main():
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        enc = os.path.join(tmp, "opus_enc_cli")
        dec = os.path.join(tmp, "opus_dec_cli")
        r = run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
                 os.path.join(REPO, "tools", "opus_enc_cli.cpp")] +
                [os.path.join(REPO, "src", s) for s in ENC_SRCS] +
                ["-o", enc])
        if r.returncode:
            sys.exit("enc build failed:\n" + r.stderr.decode()[:1200])
        r = run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
                 os.path.join(REPO, "tools", "opus_dec_cli.cpp")] +
                [os.path.join(REPO, "src", s) for s in DEC_SRCS] +
                ["-o", dec])
        if r.returncode:
            sys.exit("dec build failed:\n" + r.stderr.decode()[:1200])

        failures = 0
        cases = ((1, 64000, 200, False), (1, 96000, 100, False),
                 (1, 128000, 200, False), (2, 96000, 200, False),
                 (2, 128000, 100, False), (2, 192000, 200, False),
                 (1, 128000, 50, False), (2, 128000, 25, False),
                 (1, 96000, 200, True), (2, 96000, 200, True),
                 (2, 192000, 100, True))
        for channels, rate, msx10, vbr in cases:
            sig = os.path.join(tmp, f"sig{channels}.raw")
            if not os.path.exists(sig):
                gen_signal(sig, channels)
            bit = os.path.join(tmp, "g.bit")
            cmd = [enc, sig, str(channels), str(rate), str(msx10), bit]
            if vbr:
                cmd.append("vbr")
            r = run(cmd)
            if r.returncode:
                print(f"FAIL ch={channels} {rate} {msx10}: encoder "
                      f"error: {r.stderr.decode()[:200]}")
                failures += 1
                continue
            # 1) libopus decodes AND verifies our final ranges.
            libout = os.path.join(tmp, "lib.raw")
            r = run([OPUS_DEMO, "-d", "48000", str(channels), bit, libout])
            demo_err = (r.stdout + r.stderr).decode()
            range_fail = ("mismatch" in demo_err.lower() or
                          "error" in demo_err.lower() or r.returncode != 0)
            # 2) glint's own decoder agrees with libopus.
            mine = os.path.join(tmp, "mine.raw")
            r2 = run([dec, bit, str(channels), mine])
            a = open(libout, "rb").read()
            b = open(mine, "rb").read()
            worst = -1
            if len(a) == len(b):
                worst = max(abs(struct.unpack_from("<h", a, i)[0] -
                                struct.unpack_from("<h", b, i)[0])
                            for i in range(0, len(a), 2))
            decoders_agree = len(a) == len(b) and worst <= 4 and \
                r2.returncode == 0 and b"0 range mismatches" in r2.stdout
            # 3) SNR floor vs source.
            src = open(sig, "rb").read()
            q = snr(src, a, channels)
            ok = (not range_fail) and decoders_agree and q > 12.0
            failures += 0 if ok else 1
            print(f"{'OK' if ok else 'FAIL'} ch={channels} rate={rate} "
                  f"frame={msx10/10}ms{' vbr' if vbr else ''}: "
                  f"libopus-ranges="
                  f"{'ok' if not range_fail else 'MISMATCH'}, "
                  f"decoders-agree={worst} LSB, snr={q:.1f} dB")
        if failures:
            sys.exit(f"FAIL: {failures} cases")
        print("PASS: glint CELT encoder produces conformant streams "
              "(libopus-verified ranges, decoder agreement, sane quality)")


if __name__ == "__main__":
    sys.exit(main())
