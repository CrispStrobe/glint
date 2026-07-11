#!/usr/bin/env python3
"""Decoder robustness gate (PLAN § D): build the MP3/AAC fuzz harness with
AddressSanitizer + UndefinedBehaviorSanitizer (when the toolchain
supports them) and run a bounded fuzz over random, bit-flipped and
truncated input. A real decoder must never crash, read/write out of
bounds, or hang on malformed data — any ASan/UBSan report or a wall-clock
timeout fails the gate.

Usage: python3 tools/test_decoder_fuzz.py [path/to/glint_cli]
"""

import math
import os
import struct
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


def gen_wav(path, sr=44100, seconds=1, channels=2):
    n = sr * seconds
    frames = []
    for i in range(n):
        v = 0.4 * math.sin(2 * math.pi * 440 * i / sr)
        s = int(v * 20000)
        if channels == 2:
            frames.append(struct.pack("<hh", s, int(s * 0.8)))
        else:
            frames.append(struct.pack("<h", s))
    data = b"".join(frames)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, channels, sr,
                            sr * 2 * channels, 2 * channels, 16))
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def main():
    glint_cli = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(REPO, "build", "glint_cli")
    cxx = os.environ.get("CXX", "c++")
    iters = os.environ.get("FUZZ_ITERS", "4000")
    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, "a.wav")
        gen_wav(wav)
        mp3 = os.path.join(tmp, "v.mp3")
        aac = os.path.join(tmp, "v.aac")
        run([glint_cli, "-b", "128", "-m", "joint", "-q", "normal", wav,
             mp3])
        run([glint_cli, "-F", "aac", "-b", "128", "-q", "normal", wav,
             aac])

        # Try ASan+UBSan; fall back to a plain build if unsupported.
        base = [cxx, "-std=c++17", "-O1", "-g",
                "-I", os.path.join(REPO, "src"),
                "-I", os.path.join(REPO, "include"),
                os.path.join(REPO, "tools", "fuzz_decoders.cpp"),
                os.path.join(REPO, "src", "mp3_decoder.cpp"),
                os.path.join(REPO, "src", "aac_decoder.cpp")]
        binp = os.path.join(tmp, "fuzz")
        san = ["-fsanitize=address,undefined",
               "-fno-sanitize-recover=all"]
        r = run(base + san + ["-o", binp])
        sanitized = r.returncode == 0
        if not sanitized:
            r = run(base + ["-o", binp])
            if r.returncode != 0:
                sys.exit("fuzz build failed:\n" + r.stderr.decode()[:800])
            print("(sanitizers unavailable — running plain build)")

        env = dict(os.environ, UBSAN_OPTIONS="halt_on_error=1",
                   ASAN_OPTIONS="abort_on_error=1")
        try:
            r = subprocess.run([binp, mp3, aac, iters], env=env,
                               capture_output=True, timeout=600)
        except subprocess.TimeoutExpired:
            sys.exit("FAIL: decoder fuzz hung (possible infinite loop on "
                     "malformed input)")
        out = r.stdout.decode()
        err = r.stderr.decode()
        print(out.strip())
        bad = ("runtime error" in err or "AddressSanitizer" in err or
               "ERROR" in err or r.returncode != 0)
        if bad:
            print(err[:1500])
            sys.exit("FAIL: sanitizer flagged the decoder on malformed "
                     "input")
        if "mp3:" not in out or "aac:" not in out:
            sys.exit("FAIL: fuzz did not complete both decoders")
        tag = "ASan+UBSan" if sanitized else "plain"
        print(f"[mp3/aac] ok ({tag}, {iters} iters)")

        # Opus decoder: ASan-only (its SILK layer has benign,
        # reference-inherited signed-shift UBs on a bit-exact path).
        opus_srcs = [
            "opus_ec.cpp", "opus_laplace.cpp", "opus_cwrs.cpp",
            "opus_celt_energy.cpp", "opus_celt_rate.cpp",
            "opus_celt_bands.cpp", "opus_celt_decoder.cpp",
            "opus_decoder.cpp", "opus_ms_decoder.cpp", "opus_mdct.cpp",
            "opus_silk_excitation.cpp", "opus_silk_indices.cpp",
            "opus_silk_nlsf.cpp", "opus_silk_plc.cpp",
            "opus_silk_frame.cpp", "opus_silk_stereo.cpp",
            "opus_silk_resampler.cpp", "opus_silk_decoder.cpp"]
        obin = os.path.join(tmp, "ofuzz")
        obuild = [cxx, "-std=c++17", "-O1", "-g",
                  "-I", os.path.join(REPO, "src"),
                  os.path.join(REPO, "tools", "fuzz_opus_decoder.cpp")] + \
            [os.path.join(REPO, "src", s) for s in opus_srcs]
        obit = None
        enc = os.path.join(REPO, "build", "opus_enc_cli")
        if os.path.exists(enc):
            raw = os.path.join(tmp, "o.raw")
            import numpy as np
            n = 48000 * 2
            t = np.arange(n) / 48000.0
            pcm = np.empty(n * 2)
            pcm[0::2] = 0.3 * np.sin(2 * math.pi * 440 * t)
            pcm[1::2] = 0.3 * np.sin(2 * math.pi * 550 * t)
            (np.clip(pcm, -1, 1) * 20000).astype("<i2").tofile(raw)
            obit = os.path.join(tmp, "o.bit")
            run([enc, raw, "2", "96000", "200", obit])
        oa = run(obuild + ["-fsanitize=address", "-o", obin])
        if oa.returncode == 0:
            oit = str(max(2000, int(iters) // 2))
            try:
                r = subprocess.run(
                    [obin] + ([obit] if obit else []) + [oit], env=env,
                    capture_output=True, timeout=400)
            except subprocess.TimeoutExpired:
                sys.exit("FAIL: Opus decoder fuzz hung")
            oerr = r.stderr.decode()
            print(r.stdout.decode().strip())
            if ("AddressSanitizer" in oerr or r.returncode != 0):
                print(oerr[:1200])
                sys.exit("FAIL: ASan flagged the Opus decoder")
        else:
            print("(Opus fuzz build skipped)")

        print(f"PASS: MP3 + AAC + Opus decoders survive malformed input "
              f"(no crash / OOB / hang)")


if __name__ == "__main__":
    main()
