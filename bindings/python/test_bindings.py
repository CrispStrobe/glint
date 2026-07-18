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
import subprocess
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


class TestOpus(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            glint.load_library()
        except Exception:
            raise unittest.SkipTest("libglint not found")
        if not hasattr(glint, "OpusEncoder"):
            raise unittest.SkipTest("Opus API not in this binding")

    def test_roundtrip_final_range(self):
        """Encoder and decoder final ranges must be identical on every
        packet — the Opus conformance identity."""
        import math
        enc = glint.OpusEncoder(channels=2, bitrate=96000)
        dec = glint.OpusDecoder(channels=2, sample_rate=48000)
        for f in range(5):
            pcm = []
            for i in range(960):
                t = (f * 960 + i) / 48000.0
                pcm += [0.4 * math.sin(2 * math.pi * 440 * t),
                        0.3 * math.sin(2 * math.pi * 660 * t)]
            pkt = enc.encode(pcm)
            self.assertGreater(len(pkt), 1)
            out = dec.decode(pkt)
            n = len(out) if not hasattr(out, "shape") else out.shape[0]
            self.assertIn(n, (960, 960 * 2))  # numpy (960,2) vs flat list
            self.assertEqual(enc.final_range(), dec.final_range())
        enc.close()
        dec.close()

    def test_concealment_and_rates(self):
        dec = glint.OpusDecoder(channels=1, sample_rate=16000)
        lost = dec.decode_lost(320)
        n = len(lost) if not hasattr(lost, "shape") else lost.shape[0]
        self.assertEqual(n, 320)
        dec.close()

    def test_invalid_config(self):
        with self.assertRaises(glint.ConfigError):
            glint.OpusDecoder(channels=3)
        with self.assertRaises(glint.ConfigError):
            glint.OpusEncoder(channels=1, bitrate=100)


class TestBucketsAB(unittest.TestCase):
    """CLI feature-parity helpers: resample, whole-file decode, WAV I/O,
    transcode (PLAN buckets A+B)."""

    @classmethod
    def setUpClass(cls):
        cls.build = _build_dir()
        if glint._find_library(cls.build) is None:
            raise unittest.SkipTest("libglint not found")
        glint.set_library_path(cls.build)
        cls.lib = glint.load_library(cls.build)
        if not hasattr(cls.lib, "glint_decode_audio"):
            raise unittest.SkipTest("libglint has no decode/resample ABI")

    def _sine_wav(self, path, sr=44100, seconds=1, ch=2):
        n = sr * seconds
        pcm = []
        for i in range(n):
            v = 0.4 * math.sin(2 * math.pi * 440 * i / sr)
            pcm.extend([v] * ch)
        glint.write_wav(path, pcm, sr, ch)
        return pcm

    def test_resample_length_and_passthrough(self):
        n = 4410
        pcm = [math.sin(2 * math.pi * 200 * i / 44100) for i in range(n)]
        up = glint.resample(pcm, 44100, 48000, channels=1)
        got = len(up) if not hasattr(up, "shape") else up.shape[0]
        self.assertAlmostEqual(got, round(n * 48000 / 44100), delta=2)
        same = glint.resample(pcm, 44100, 44100, channels=1)
        gsame = len(same) if not hasattr(same, "shape") else same.shape[0]
        self.assertEqual(gsame, n)

    def test_resample_preserves_amplitude(self):
        n = 8820
        pcm = [0.5 * math.sin(2 * math.pi * 300 * i / 44100)
               for i in range(n)]
        out = glint.resample(pcm, 44100, 22050, channels=1)
        vals = out.tolist() if hasattr(out, "tolist") else out
        peak = max(abs(x) for x in vals[100:-100])
        self.assertGreater(peak, 0.45)
        self.assertLess(peak, 0.55)

    def test_wav_roundtrip(self):
        with tempfile.TemporaryDirectory() as d:
            p = os.path.join(d, "s.wav")
            self._sine_wav(p, sr=44100, ch=2)
            pcm, sr, ch = glint.read_wav_float(p)
            self.assertEqual((sr, ch), (44100, 2))
            frames = pcm.shape[0] if hasattr(pcm, "shape") else len(pcm) // ch
            self.assertEqual(frames, 44100)

    def test_wav_bit_depths(self):
        # Write 8/16/24/32-int and 32/64-float; read each back and check
        # the round-trip amplitude survives (24-bit/float lossless-ish).
        n, ch = 4096, 1
        pcm = [0.5 * math.sin(2 * math.pi * 300 * i / 44100)
               for i in range(n)]
        with tempfile.TemporaryDirectory() as d:
            for bits, flt, tol in [(8, False, 0.02), (16, False, 2e-4),
                                   (24, False, 1e-5), (32, False, 1e-6),
                                   (32, True, 1e-6), (64, True, 1e-6)]:
                p = os.path.join(d, f"w{bits}{'f' if flt else ''}.wav")
                glint.write_wav(p, pcm, 44100, ch, bits=bits, float_fmt=flt)
                back, sr, bch = glint.read_wav_float(p)
                self.assertEqual((sr, bch), (44100, 1))
                vals = back.reshape(-1).tolist() if hasattr(back, "reshape") \
                    else list(back)
                err = max(abs(a - b) for a, b in zip(pcm, vals))
                self.assertLess(err, tol,
                                f"{bits}-bit{'f' if flt else ''} err {err}")

    def test_decode_file_mp3_and_aac(self):
        with tempfile.TemporaryDirectory() as d:
            wav = os.path.join(d, "s.wav")
            self._sine_wav(wav, sr=44100, ch=2)
            for ext in ("mp3", "aac"):
                out = os.path.join(d, f"a.{ext}")
                try:
                    glint.transcode_file(wav, out, bitrate=128)
                except glint.GlintError as e:
                    self.skipTest(str(e))
                self.assertGreater(os.path.getsize(out), 1000)
                pcm, sr, ch = glint.decode_file(out)
                self.assertEqual(ch, 2)
                self.assertIn(sr, (44100, 48000))
                frames = pcm.shape[0] if hasattr(pcm, "shape") \
                    else len(pcm) // ch
                self.assertGreater(frames, 40000)

    def test_transcode_with_rate(self):
        with tempfile.TemporaryDirectory() as d:
            wav = os.path.join(d, "s.wav")
            self._sine_wav(wav, sr=44100, ch=2)
            out = os.path.join(d, "r.wav")
            glint.transcode_file(wav, out, rate=48000)
            pcm, sr, ch = glint.decode_file(out)
            self.assertEqual(sr, 48000)

    def test_encode_opus_and_roundtrip(self):
        if not hasattr(self.lib, "glint_opus_encode_file"):
            self.skipTest("libglint has no Opus file encoder")
        # 48 kHz float sine -> encode_opus_file -> decode_audio
        n, ch = 48000, 2
        pcm = []
        for i in range(n):
            v = 0.4 * math.sin(2 * math.pi * 440 * i / 48000)
            pcm.extend([v] * ch)
        data = glint.encode_opus_file(pcm, ch, bitrate=96000)
        self.assertTrue(data[:4] == b"OggS" and len(data) > 1000)
        out, sr, dch = glint.decode_bytes(data)
        self.assertEqual((sr, dch), (48000, 2))
        frames = out.shape[0] if hasattr(out, "shape") else len(out) // dch
        self.assertGreater(frames, 40000)

    def test_encoder_knobs(self):
        # MP3 Encoder with explicit mode/quality/VBR must produce a decodable
        # stream; AAC VBR too.
        n = 44100
        sine = [int(30000 * math.sin(2 * math.pi * 440 * i / 44100))
                for i in range(n)]
        st = []
        for s in sine:
            st.extend([s, s])
        # MP3: stereo mode, best quality, VBR q3
        with glint.Encoder(sample_rate=44100, channels=2, bitrate=192,
                           mode=glint.MODE_STEREO, quality=glint.QUALITY_BEST,
                           vbr_quality=3, lib_path=self.build) as enc:
            data = glint._encode_int16(enc, st, 2) \
                if hasattr(glint, "_encode_int16") else None
            if data is None:
                spf = enc.samples_per_frame
                out = bytearray()
                for off in range(0, len(st) - spf * 2 + 1, spf * 2):
                    out.extend(enc.encode(st[off:off + spf * 2]))
                out.extend(enc.flush())
                data = bytes(out)
        pcm, sr, ch = glint.decode_bytes(data)
        self.assertEqual((sr, ch), (44100, 2))
        # AAC: VBR quality 2
        with glint.AacEncoder(sample_rate=44100, channels=2, bitrate=128,
                              vbr_quality=2, lib_path=self.build) as aac:
            spf = aac.samples_per_frame
            out = bytearray()
            for off in range(0, len(st) - spf * 2 + 1, spf * 2):
                out.extend(aac.encode(st[off:off + spf * 2]))
            out.extend(aac.flush())
        apcm, asr, ach = glint.decode_bytes(bytes(out))
        self.assertEqual(ach, 2)

    def test_encode_audio_all_codecs_odd_rate(self):
        if not hasattr(self.lib, "glint_encode_audio"):
            self.skipTest("libglint has no one-call encode")
        # 37 kHz (invalid for MP3/AAC) float PCM -> each codec, decode back.
        sr, ch, n = 37000, 2, 37000
        pcm = []
        for i in range(n):
            v = 0.4 * math.sin(2 * math.pi * 440 * i / sr)
            pcm.extend([v] * ch)
        for codec, want_sr in [("mp3", 32000), ("aac", 32000),
                               ("opus", 48000)]:
            data = glint.encode_audio(pcm, ch, sr, codec, bitrate=128)
            self.assertGreater(len(data), 1000, codec)
            out, dsr, dch = glint.decode_bytes(data)
            self.assertEqual(dsr, want_sr, f"{codec} rate")
            self.assertEqual(dch, 2, f"{codec} channels")

    def test_decode_dtype_and_rate(self):
        # Encode a sine to MP3, then decode float/int16 and resampled.
        n, ch = 44100, 2
        pcm = []
        for i in range(n):
            v = 0.4 * math.sin(2 * math.pi * 440 * i / 44100)
            pcm.extend([v] * ch)
        mp3 = glint.encode_audio(pcm, ch, 44100, "mp3", bitrate=128)
        fpcm, sr, c = glint.decode_bytes(mp3)  # float native
        self.assertEqual((sr, c), (44100, 2))
        ipcm, sr2, c2 = glint.decode_bytes(mp3, dtype="int16")
        self.assertEqual((sr2, c2), (44100, 2))
        if hasattr(ipcm, "dtype"):
            self.assertEqual(str(ipcm.dtype), "int16")
        rpcm, sr3, c3 = glint.decode_bytes(mp3, rate=24000)
        self.assertEqual(sr3, 24000)
        rframes = rpcm.shape[0] if hasattr(rpcm, "shape") else len(rpcm) // c3
        exp = (fpcm.shape[0] if hasattr(fpcm, "shape") else len(fpcm) // c)
        self.assertLess(abs(rframes - exp * 24000 / 44100), 50)

    def test_decode_opus_surround(self):
        # 5.1 Opus via ffmpeg+libopus -> glint decodes 6 channels.
        import shutil
        if not shutil.which("ffmpeg"):
            self.skipTest("ffmpeg not available")
        with tempfile.TemporaryDirectory() as d:
            op = os.path.join(d, "s.opus")
            r = subprocess.run(
                ["ffmpeg", "-y", "-v", "error", "-f", "lavfi",
                 "-i", "sine=440:d=1",
                 "-af", "aformat=channel_layouts=5.1",
                 "-c:a", "libopus", "-mapping_family", "1", op],
                capture_output=True)
            if r.returncode != 0 or not os.path.exists(op):
                self.skipTest("ffmpeg has no libopus / 5.1 encode failed")
            pcm, sr, ch = glint.decode_file(op)
            self.assertEqual(sr, 48000)
            self.assertEqual(ch, 6)
            frames = pcm.shape[0] if hasattr(pcm, "shape") else len(pcm) // ch
            self.assertGreater(frames, 40000)

    def test_transcode_to_opus(self):
        if not hasattr(self.lib, "glint_opus_encode_file"):
            self.skipTest("libglint has no Opus file encoder")
        with tempfile.TemporaryDirectory() as d:
            wav = os.path.join(d, "s.wav")
            self._sine_wav(wav, sr=44100, ch=2)  # 44.1k -> auto 48k
            out = os.path.join(d, "a.opus")
            glint.transcode_file(wav, out, bitrate=96)
            self.assertGreater(os.path.getsize(out), 1000)
            pcm, sr, ch = glint.decode_file(out)
            self.assertEqual((sr, ch), (48000, 2))



class VorbisDecodeTest(unittest.TestCase):
    """Vorbis decode through the whole-file auto-detect (C detect splits
    OggS into Opus vs Vorbis by codec id)."""
    _VORBIS_OGG_B64 = (
        "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"
    )

    @classmethod
    def setUpClass(cls):
        cls.build = _build_dir()
        if glint._find_library(cls.build) is None:
            raise unittest.SkipTest("libglint not found")
        glint.set_library_path(cls.build)
        cls.lib = glint.load_library(cls.build)
        if not hasattr(cls.lib, "glint_decode_audio"):
            raise unittest.SkipTest("libglint has no whole-file decode ABI")

    def test_decode_vorbis_autodetect(self):
        # A real libvorbis stream (sox -C 3, 44.1k mono, 440 Hz), embedded so
        # the test is self-contained. decode_bytes must auto-detect Vorbis
        # (both Opus and Vorbis are OggS; the C detect splits by codec id).
        import base64
        ogg = base64.b64decode("".join(self._VORBIS_OGG_B64.split()))
        pcm, sr, ch = glint.decode_bytes(ogg)
        self.assertEqual((sr, ch), (44100, 1))
        frames = pcm.shape[0] if hasattr(pcm, "shape") else len(pcm) // ch
        self.assertGreater(frames, 20000)

    def test_decode_vorbis_dedicated(self):
        # The dedicated whole-buffer entry point, when the wrapper exposes it.
        import base64
        ogg = base64.b64decode("".join(self._VORBIS_OGG_B64.split()))
        if not hasattr(self.lib, "glint_vorbis_decode"):
            self.skipTest("libglint has no glint_vorbis_decode symbol")
        # Symbol must be present and callable through ctypes.
        self.assertTrue(callable(self.lib.glint_vorbis_decode))


if __name__ == "__main__":
    # Strip our custom argv before unittest parses it
    build = _build_dir()
    if len(sys.argv) > 1:
        sys.argv = sys.argv[:1]
    unittest.main(verbosity=2)
