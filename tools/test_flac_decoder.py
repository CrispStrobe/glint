#!/usr/bin/env python3
"""FLAC decoder gate: generate native FLAC streams with ffmpeg, decode them
with glint_cli, and compare the resulting PCM against the source WAV."""

import math
import os
import struct
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def run(cmd):
    return subprocess.run(cmd, capture_output=True)


def write_wav(path, sr, ch, bits, seconds):
    n = int(sr * seconds)
    samples = []
    peak = (1 << (bits - 1)) - 1
    for i in range(n):
        t = i / sr
        vals = [
            0.45 * math.sin(2 * math.pi * 440 * t),
            0.31 * math.sin(2 * math.pi * 661 * t) +
            0.06 * math.sin(2 * math.pi * 97 * t),
        ]
        for c in range(ch):
            samples.append(int(max(-1, min(1, vals[c])) * peak))
    if bits == 16:
        data = struct.pack("<" + "h" * len(samples), *samples)
        align = ch * 2
    elif bits == 24:
        data = b"".join((v & 0xFFFFFF).to_bytes(3, "little") for v in samples)
        align = ch * 3
    else:
        raise ValueError(bits)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt ")
        f.write(struct.pack("<IHHIIHH", 16, 1, ch, sr, sr * align, align, bits))
        f.write(b"data" + struct.pack("<I", len(data)) + data)
    return samples


def read_wav_i16(path):
    d = open(path, "rb").read()
    i = d.find(b"data")
    if i < 0:
        raise ValueError("no data chunk")
    n = struct.unpack("<I", d[i + 4:i + 8])[0]
    return struct.unpack("<%dh" % (n // 2), d[i + 8:i + 8 + n])


def read_wav_f32(path):
    d = open(path, "rb").read()
    i = d.find(b"data")
    if i < 0:
        raise ValueError("no data chunk")
    n = struct.unpack("<I", d[i + 4:i + 8])[0]
    return struct.unpack("<%df" % (n // 4), d[i + 8:i + 8 + n])


def check_case(cli, tmp, name, sr, ch, bits):
    wav = os.path.join(tmp, name + ".wav")
    flac = os.path.join(tmp, name + ".flac")
    out = os.path.join(tmp, name + "_out.wav")
    src = write_wav(wav, sr, ch, bits, 0.75)
    r = run(["ffmpeg", "-y", "-v", "error", "-i", wav,
             "-compression_level", "8", flac])
    assert r.returncode == 0, r.stderr.decode()
    r = run([cli, "--info", flac])
    info = r.stderr.decode()
    assert r.returncode == 0 and f"{sr} Hz" in info and f"{ch} ch" in info, info
    cmd = [cli]
    if bits == 24:
        cmd += ["--wav-float", "--bits", "32"]
    cmd += [flac, out]
    r = run(cmd)
    assert r.returncode == 0, r.stderr.decode()
    if bits == 16:
        a = read_wav_i16(wav)
        b = read_wav_i16(out)
        assert len(a) == len(b), (len(a), len(b))
        maxerr = max(abs(x - y) for x, y in zip(a, b))
        den = sum((x - y) * (x - y) for x, y in zip(a, b))
        num = sum(x * x for x in a)
        snr = float("inf") if den == 0 else 10 * math.log10(num / den)
        print(f"{name}: samples={len(a)} maxerr_lsb={maxerr} snr_db={snr}")
        assert maxerr <= 1
    else:
        b = read_wav_f32(out)
        assert len(src) == len(b), (len(src), len(b))
        scale = float(1 << 23)
        maxerr = max(abs(x / scale - y) for x, y in zip(src, b))
        print(f"{name}: samples={len(src)} maxerr_float={maxerr:.3g}")
        assert maxerr < 1e-7


def main():
    cli = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, "build", "glint_cli")
    with tempfile.TemporaryDirectory() as tmp:
        check_case(cli, tmp, "stereo16", 48000, 2, 16)
        check_case(cli, tmp, "mono24", 32000, 1, 24)
    print("PASS: FLAC decode")


if __name__ == "__main__":
    main()
