#!/usr/bin/env python3
"""
Example: generate a 440 Hz sine wave and encode it to MP3 with glint.

Usage:
    python example.py [build_dir]

If build_dir is not given, the script looks for libglint.so in ../../build
relative to this file.
"""

import math
import os
import struct
import sys

# Allow running from the bindings/python directory
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glint


def generate_sine(freq_hz=440, sample_rate=44100, duration_s=2.0, amplitude=30000):
    """Generate a mono 16-bit PCM sine wave as a list of int values."""
    n_samples = int(sample_rate * duration_s)
    samples = []
    for i in range(n_samples):
        t = i / sample_rate
        value = int(amplitude * math.sin(2 * math.pi * freq_hz * t))
        # Clamp to int16 range
        value = max(-32768, min(32767, value))
        samples.append(value)
    return samples


def write_wav(path, samples, sample_rate=44100, channels=1):
    """Write a minimal 16-bit PCM WAV file."""
    n_samples = len(samples)
    data_size = n_samples * 2
    fmt_chunk = struct.pack("<HHIIHH", 1, channels, sample_rate,
                            sample_rate * channels * 2, channels * 2, 16)
    with open(path, "wb") as f:
        f.write(b"RIFF")
        f.write(struct.pack("<I", 36 + data_size))
        f.write(b"WAVE")
        f.write(b"fmt ")
        f.write(struct.pack("<I", len(fmt_chunk)))
        f.write(fmt_chunk)
        f.write(b"data")
        f.write(struct.pack("<I", data_size))
        for s in samples:
            f.write(struct.pack("<h", s))


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else None
    if build_dir:
        glint.set_library_path(build_dir)

    sample_rate = 44100
    duration = 2.0
    bitrate = 128

    print(f"Generating {duration}s 440 Hz sine wave at {sample_rate} Hz ...")
    pcm = generate_sine(440, sample_rate, duration)

    # -- Method 1: streaming encoder --
    print(f"Encoding with streaming Encoder (bitrate={bitrate}) ...")
    mp3_data = bytearray()
    with glint.Encoder(sample_rate=sample_rate, channels=1, bitrate=bitrate,
                       lib_path=build_dir) as enc:
        spf = enc.samples_per_frame
        offset = 0
        frames = 0
        while offset + spf <= len(pcm):
            chunk = pcm[offset:offset + spf]
            mp3_data.extend(enc.encode(chunk))
            offset += spf
            frames += 1
        mp3_data.extend(enc.flush())

    out_path = "sine_440.mp3"
    with open(out_path, "wb") as f:
        f.write(mp3_data)
    print(f"  Wrote {out_path}: {len(mp3_data)} bytes ({frames} frames encoded)")

    # -- Method 2: one-shot convenience function --
    print("Encoding with encode_pcm() convenience function ...")
    mp3_oneshot = glint.encode_pcm(pcm, sample_rate=sample_rate, bitrate=bitrate,
                                   lib_path=build_dir)
    out2 = "sine_440_oneshot.mp3"
    with open(out2, "wb") as f:
        f.write(mp3_oneshot)
    print(f"  Wrote {out2}: {len(mp3_oneshot)} bytes")

    # -- Method 3: WAV file round-trip --
    wav_path = "sine_440.wav"
    print(f"Writing temporary WAV: {wav_path} ...")
    write_wav(wav_path, pcm, sample_rate)
    out3 = "sine_440_fromwav.mp3"
    glint.encode_file(wav_path, out3, bitrate=bitrate, lib_path=build_dir)
    mp3_size = os.path.getsize(out3)
    print(f"  Wrote {out3}: {mp3_size} bytes")

    print("Done!")


if __name__ == "__main__":
    main()
