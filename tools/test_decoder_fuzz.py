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
        print(f"PASS: MP3 + AAC decoders survive malformed input ({tag}, "
              f"{iters} iters x random/bitflip/truncate)")


if __name__ == "__main__":
    main()
