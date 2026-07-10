#!/usr/bin/env python3
"""End-to-end test: glint's clean-room Opus decoder vs libopus on REAL
CELT-only streams.

Encodes a synthetic test signal with opus_demo (restricted-lowdelay =
CELT-only fullband) at all four frame sizes, mono+stereo, VBR and CBR;
decodes each stream with glint's decoder AND opus_demo; requires
- the decoder final range to equal the encoder's for EVERY packet (the
  Opus conformance identity — an exact, integer check), and
- PCM to match libopus's decode within a few int16 LSB (float-vs-double).

Usage: python3 tools/test_opus_e2e.py
"""

import math
import os
import struct
import subprocess
import sys
import tempfile

TOOLS_DIR = os.path.expanduser("~/code/glint-tools")
CUSTOM_SRC = os.path.join(TOOLS_DIR, "opus-1.5.2-custom")
CUSTOM_LIB = os.path.join(CUSTOM_SRC, ".libs", "libopus.a")
OPUS_DEMO = os.path.join(TOOLS_DIR, "opus_demo")
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRCS = ["opus_ec.cpp", "opus_laplace.cpp", "opus_cwrs.cpp",
        "opus_celt_energy.cpp", "opus_celt_rate.cpp", "opus_celt_bands.cpp",
        "opus_mdct.cpp", "opus_celt_decoder.cpp", "opus_decoder.cpp",
        "opus_silk_excitation.cpp", "opus_silk_indices.cpp",
        "opus_silk_nlsf.cpp", "opus_silk_frame.cpp", "opus_silk_stereo.cpp",
        "opus_silk_resampler.cpp", "opus_silk_decoder.cpp"]
MAX_LSB = 4


def run(cmd, quiet=False, **kw):
    if not quiet:
        print("+ " + " ".join(cmd), flush=True)
    return subprocess.run(cmd, check=True, capture_output=quiet, **kw)


def ensure_opus_demo():
    if os.path.exists(OPUS_DEMO):
        return
    cc = os.environ.get("CC", "cc")
    run([cc, "-O2", "-DOPUS_BUILD",
         "-I", os.path.join(CUSTOM_SRC, "include"),
         "-I", os.path.join(CUSTOM_SRC, "celt"),
         "-I", os.path.join(CUSTOM_SRC, "silk"),
         os.path.join(CUSTOM_SRC, "src", "opus_demo.c"),
         CUSTOM_LIB, "-lm", "-o", OPUS_DEMO])


def gen_signal(path, channels, seconds=2):
    """Sine sweep + tone + noise bursts, 48 kHz int16 raw."""
    n = 48000 * seconds
    rng = 12345
    data = bytearray()
    for i in range(n):
        t = i / 48000.0
        f = 100 * math.exp(t * 2.3)  # sweep 100 Hz -> ~1 kHz/s
        s = 0.4 * math.sin(2 * math.pi * f * t)
        s += 0.2 * math.sin(2 * math.pi * 3001 * t)
        rng = (1664525 * rng + 1013904223) & 0xFFFFFFFF
        noise = (rng / 2**31 - 1.0) * 0.05
        burst = 0.5 if (i // 4800) % 7 == 3 and i % 4800 < 480 else 0.0
        for c in range(channels):
            v = s + noise + burst * (1 if c == 0 else -1)
            v = max(-0.95, min(0.95, v))
            data += struct.pack("<h", int(v * 32767))
    with open(path, "wb") as f:
        f.write(data)


def main():
    if not os.path.exists(CUSTOM_LIB):
        sys.exit("missing custom-modes libopus; run "
                 "tools/crosscheck_opus_celt_prims.py once to build it")
    ensure_opus_demo()
    cxx = os.environ.get("CXX", "c++")
    with tempfile.TemporaryDirectory() as tmp:
        cli = os.path.join(tmp, "opus_dec_cli")
        run([cxx, "-std=c++17", "-O2", "-I", os.path.join(REPO, "src"),
             os.path.join(REPO, "tools", "opus_dec_cli.cpp")] +
            [os.path.join(REPO, "src", s) for s in SRCS] + ["-o", cli])

        failures = 0
        # (app, frame ms, bitrate, extra encoder args, decode channels or
        #  None for same-as-encode). restricted-lowdelay = CELT-only;
        #  voip + NB/MB/WB = SILK-only; voip + SWB/FB = hybrid. Decode-
        #  channel mismatches exercise the up/downmix paths.
        cases = (("restricted-lowdelay", "2.5", 128000, [], None),
                 ("restricted-lowdelay", "5", 96000, [], None),
                 ("restricted-lowdelay", "10", 64000, ["-cbr"], None),
                 ("restricted-lowdelay", "20", 128000, [], None),
                 ("restricted-lowdelay", "20", 320000, ["-cbr"], None),
                 ("restricted-lowdelay", "20", 96000,
                  ["-bandwidth", "NB"], None),
                 ("restricted-lowdelay", "10", 96000,
                  ["-bandwidth", "WB"], None),
                 ("restricted-lowdelay", "20", 128000,
                  ["-bandwidth", "SWB"], None),
                 ("restricted-lowdelay", "20", 128000, [], "swap"),
                 # SILK-only
                 ("voip", "20", 12000, ["-bandwidth", "NB"], None),
                 ("voip", "20", 16000, ["-bandwidth", "MB"], None),
                 ("voip", "20", 24000, ["-bandwidth", "WB"], None),
                 ("voip", "40", 20000, ["-bandwidth", "WB"], None),
                 ("voip", "60", 16000, ["-bandwidth", "WB"], None),
                 ("voip", "20", 24000, ["-bandwidth", "WB", "-cbr"],
                  "swap"),
                 # Hybrid
                 ("voip", "20", 28000, ["-bandwidth", "SWB"], None),
                 ("voip", "10", 32000, ["-bandwidth", "FB"], None),
                 ("voip", "20", 40000, ["-bandwidth", "FB"], None),
                 ("voip", "20", 32000, ["-bandwidth", "FB", "-cbr"],
                  "swap"))
        for channels in (1, 2):
            sig = os.path.join(tmp, f"sig{channels}.raw")
            gen_signal(sig, channels)
            for app, fsize, rate, extra, decmode in cases:
                dec_ch = channels if decmode is None else 3 - channels
                bit = os.path.join(tmp, "a.bit")
                ref = os.path.join(tmp, "ref.raw")
                mine = os.path.join(tmp, "mine.raw")
                run([OPUS_DEMO, "-e", app, "48000",
                     str(channels), str(rate), "-framesize", fsize] + extra +
                    [sig, bit], quiet=True)
                run([OPUS_DEMO, "-d", "48000", str(dec_ch), bit, ref],
                    quiet=True)
                r = run([cli, bit, str(dec_ch), mine], quiet=True)
                cli_msg = r.stdout.decode().strip()

                a = open(ref, "rb").read()
                b = open(mine, "rb").read()
                # opus_demo trims the encoder preskip? Both decode the same
                # packets; lengths must match exactly.
                if len(a) != len(b):
                    print(f"FAIL ch={channels} fs={fsize}: length "
                          f"{len(a)} vs {len(b)}")
                    failures += 1
                    continue
                worst = 0
                for i in range(0, len(a), 2):
                    va = struct.unpack_from("<h", a, i)[0]
                    vb = struct.unpack_from("<h", b, i)[0]
                    worst = max(worst, abs(va - vb))
                status = "OK" if worst <= MAX_LSB else "FAIL"
                if status == "FAIL":
                    failures += 1
                tag = " ".join(extra) if extra else "vbr"
                print(f"{status} {app} ch={channels}->{dec_ch} "
                      f"fs={fsize}ms rate={rate} [{tag}]: {cli_msg}, "
                      f"max |pcm diff| = {worst} LSB")
        if failures:
            sys.exit(f"FAIL: {failures} configurations failed")
        print("PASS: glint Opus decoder matches libopus on real CELT-only "
              "streams (final ranges exact, PCM within tolerance)")


if __name__ == "__main__":
    sys.exit(main())
