#!/usr/bin/env python3
"""
Tests for glint Python bindings.

Usage:
    python test_bindings.py [build_dir]

build_dir defaults to ../../build relative to this script.
"""

import math
import os
import struct
import sys
import tempfile
import unittest

# Ensure the bindings module is importable
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glint


def _build_dir():
    """Return the build directory from argv or a default relative path."""
    if len(sys.argv) > 1:
        return sys.argv[1]
    return os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "..", "..", "build")


def _make_silence(n):
    """Return a list of n zero-valued int16 samples."""
    return [0] * n


def _make_sine(n, freq=440, sr=44100, amp=30000):
    """Return a list of n int16 samples of a sine wave."""
    return [int(amp * math.sin(2 * math.pi * freq * i / sr)) for i in range(n)]


def _write_wav(path, samples, sample_rate=44100, channels=1):
    """Write a minimal 16-bit PCM WAV file."""
    n = len(samples)
    data_size = n * 2
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


class TestEncoder(unittest.TestCase):
    """Core encoder tests."""

    @classmethod
    def setUpClass(cls):
        cls.build = _build_dir()
        # Verify library is findable
        lib_path = glint._find_library(cls.build)
        if lib_path is None:
            raise unittest.SkipTest(
                f"libglint not found in {cls.build}; pass build dir as argv[1]"
            )

    def test_check_config_valid(self):
        glint.set_library_path(self.build)
        self.assertTrue(glint.check_config(44100, 128))

    def test_check_config_invalid(self):
        glint.set_library_path(self.build)
        self.assertFalse(glint.check_config(12345, 999))

    def test_create_and_destroy(self):
        enc = glint.Encoder(sample_rate=44100, channels=1, bitrate=128,
                            lib_path=self.build)
        self.assertGreater(enc.samples_per_frame, 0)
        enc.close()

    def test_encode_silence(self):
        """Encode several frames of silence; expect valid MP3 output."""
        with glint.Encoder(sample_rate=44100, channels=1, bitrate=128,
                           lib_path=self.build) as enc:
            spf = enc.samples_per_frame
            mp3 = bytearray()
            for _ in range(10):
                mp3.extend(enc.encode(_make_silence(spf)))
            mp3.extend(enc.flush())

        # MP3 frames start with sync word 0xFFE0..0xFFFF
        self.assertGreater(len(mp3), 0, "Expected non-empty MP3 output")
        # Check for at least one MP3 sync word
        found_sync = False
        for i in range(len(mp3) - 1):
            if mp3[i] == 0xFF and (mp3[i + 1] & 0xE0) == 0xE0:
                found_sync = True
                break
        self.assertTrue(found_sync, "No MP3 sync word found in output")

    def test_encode_sine(self):
        """Encode a 1-second sine wave; check output size is reasonable."""
        sr = 44100
        with glint.Encoder(sample_rate=sr, channels=1, bitrate=128,
                           lib_path=self.build) as enc:
            spf = enc.samples_per_frame
            pcm = _make_sine(sr)  # 1 second
            mp3 = bytearray()
            offset = 0
            while offset + spf <= len(pcm):
                mp3.extend(enc.encode(pcm[offset:offset + spf]))
                offset += spf
            mp3.extend(enc.flush())

        # 128 kbps * 1 second = ~16000 bytes; allow wide range
        self.assertGreater(len(mp3), 1000,
                           "MP3 output suspiciously small for 1s at 128kbps")
        self.assertLess(len(mp3), 100000,
                        "MP3 output suspiciously large for 1s at 128kbps")

    def test_context_manager(self):
        """Encoder used as context manager should be closed on exit."""
        with glint.Encoder(sample_rate=44100, channels=1, bitrate=128,
                           lib_path=self.build) as enc:
            self.assertIsNotNone(enc._handle)
        self.assertIsNone(enc._handle)

    def test_use_after_close_raises(self):
        enc = glint.Encoder(sample_rate=44100, channels=1, bitrate=128,
                            lib_path=self.build)
        enc.close()
        with self.assertRaises(glint.GlintError):
            enc.encode(_make_silence(1152))

    def test_double_close_is_safe(self):
        enc = glint.Encoder(sample_rate=44100, channels=1, bitrate=128,
                            lib_path=self.build)
        enc.close()
        enc.close()  # should not raise


class TestConvenienceFunctions(unittest.TestCase):
    """Tests for encode_pcm and encode_file."""

    @classmethod
    def setUpClass(cls):
        cls.build = _build_dir()
        lib_path = glint._find_library(cls.build)
        if lib_path is None:
            raise unittest.SkipTest("libglint not found")

    def test_encode_pcm(self):
        pcm = _make_sine(44100, freq=440, sr=44100)
        mp3 = glint.encode_pcm(pcm, sample_rate=44100, bitrate=128,
                                lib_path=self.build)
        self.assertGreater(len(mp3), 1000)

    def test_encode_file(self):
        sr = 44100
        pcm = _make_sine(sr)
        with tempfile.TemporaryDirectory() as tmpdir:
            wav_path = os.path.join(tmpdir, "test.wav")
            mp3_path = os.path.join(tmpdir, "test.mp3")
            _write_wav(wav_path, pcm, sr)
            glint.encode_file(wav_path, mp3_path, bitrate=128,
                              lib_path=self.build)
            mp3_size = os.path.getsize(mp3_path)
            self.assertGreater(mp3_size, 1000)

            # Verify the output starts with an MP3 sync word
            with open(mp3_path, "rb") as f:
                header = f.read(2)
            self.assertEqual(header[0], 0xFF)
            self.assertTrue(header[1] & 0xE0 == 0xE0)


class TestNumpy(unittest.TestCase):
    """Test NumPy integration (skipped if numpy is not installed)."""

    @classmethod
    def setUpClass(cls):
        cls.build = _build_dir()
        lib_path = glint._find_library(cls.build)
        if lib_path is None:
            raise unittest.SkipTest("libglint not found")
        try:
            import numpy  # noqa: F401
        except ImportError:
            raise unittest.SkipTest("numpy not available")

    def test_encode_numpy_array(self):
        import numpy as np
        sr = 44100
        t = np.arange(sr, dtype=np.float64) / sr
        pcm = (np.sin(2 * np.pi * 440 * t) * 30000).astype(np.int16)

        mp3 = glint.encode_pcm(pcm, sample_rate=sr, bitrate=128,
                                lib_path=self.build)
        self.assertGreater(len(mp3), 1000)


class TestAacEncoder(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = _build_dir()
        lib_path = glint._find_library(cls.build)
        if lib_path is None:
            raise unittest.SkipTest("libglint not found")
        lib = glint.load_library(cls.build)
        if not hasattr(lib, "glint_aac_create"):
            raise unittest.SkipTest("libglint has no AAC support")

    def test_aac_stereo_roundtrip(self):
        import math
        enc = glint.AacEncoder(sample_rate=44100, channels=2, bitrate=128,
                               lib_path=self.build)
        self.assertEqual(enc.samples_per_frame, 1024)
        spf = enc.samples_per_frame
        out = b""
        for f in range(20):
            frame = []
            for i in range(spf):
                v = int(12000 * math.sin(2 * math.pi * 440 * (f * spf + i) / 44100))
                frame.extend((v, v // 2))
            out += enc.encode(frame)
        out += enc.flush()
        enc.close()
        # 20 encode calls + 2 flush frames, each an ADTS frame
        self.assertGreater(len(out), 20 * 100)
        self.assertEqual(out[0], 0xFF)
        self.assertEqual(out[1] & 0xF6, 0xF0)

    def test_aac_invalid_config(self):
        with self.assertRaises(glint.ConfigError):
            glint.AacEncoder(sample_rate=12345, channels=2, bitrate=128,
                             lib_path=self.build)

    def test_version(self):
        lib = glint.load_library(self.build)
        v = lib.glint_version()
        self.assertGreaterEqual(v, (0 << 16) | (8 << 8))


if __name__ == "__main__":
    # Strip our custom argv before unittest parses it
    build = _build_dir()
    if len(sys.argv) > 1:
        sys.argv = sys.argv[:1]
    unittest.main(verbosity=2)
