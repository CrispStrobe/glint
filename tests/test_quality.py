#!/usr/bin/env python3
"""
glint quality test suite.

Tests encode→decode round-trip quality for various signals.
Requires: ffmpeg, glint_cli binary.

Usage:
    python3 tests/test_quality.py [path/to/glint_cli] [--speech path/to/speech.wav]

Exit code 0 = all pass, 1 = any fail.
"""

import numpy as np
import wave
import subprocess
import sys
import os
import tempfile
import argparse

SR = 44100


def gen_wav(path, pcm_int16, sr=SR, nch=1):
    """Write int16 PCM to WAV file."""
    with wave.open(path, 'w') as w:
        w.setnchannels(nch)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(np.clip(pcm_int16, -32767, 32767).astype(np.int16).tobytes())


def read_wav(path):
    """Read WAV file, return float64 array."""
    with wave.open(path, 'r') as w:
        return np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(np.float64)


def encode_decode(enc_bin, wav_in, mp3_out, dec_out, bitrate=128):
    """Encode WAV→MP3 with glint, decode MP3→WAV with ffmpeg."""
    r = subprocess.run([enc_bin, wav_in, mp3_out, '-b', str(bitrate)],
                       capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        raise RuntimeError(f"Encode failed: {r.stderr}")

    r = subprocess.run(['ffmpeg', '-y', '-i', mp3_out, '-ar', str(SR), '-ac', '1',
                        '-acodec', 'pcm_s16le', dec_out],
                       capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        raise RuntimeError(f"Decode failed: {r.stderr}")


def best_correlation(orig, dec, max_delay=4000):
    """Find best |correlation| over a range of delays."""
    bc, bd, bs = 0, 0, -99
    for d in range(0, min(max_delay, len(dec) - 5000)):
        n = min(len(orig), len(dec) - d) - 500
        if n < 1000:
            continue
        o, dv = orig[:n], dec[d:d + n]
        c = np.corrcoef(o, dv)[0, 1]
        if abs(c) > abs(bc):
            bc, bd = c, d
            bs = 10 * np.log10(np.mean(o**2) / np.mean((o - dv)**2)) if np.mean((o - dv)**2) > 0 else 99
    return bc, bs, bd


def check_ffmpeg_errors(mp3_path):
    """Run ffmpeg null decode and check for errors."""
    r = subprocess.run(['ffmpeg', '-i', mp3_path, '-f', 'null', '-'],
                       capture_output=True, text=True, timeout=30)
    errors = [l for l in r.stderr.split('\n')
              if 'error' in l.lower() and 'no error' not in l.lower()]
    return errors


def test_signal(enc_bin, name, pcm, min_corr, tmpdir, bitrate=128, enc_extra=None):
    """Encode a signal, decode, measure correlation. Return pass/fail."""
    wav = os.path.join(tmpdir, f'{name}.wav')
    mp3 = os.path.join(tmpdir, f'{name}.mp3')
    dec = os.path.join(tmpdir, f'{name}_dec.wav')

    gen_wav(wav, pcm)
    extra = enc_extra or []
    r = subprocess.run([enc_bin, wav, mp3, '-b', str(bitrate)] + extra,
                       capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        print(f"  FAIL {name}: encode failed: {r.stderr[:200]}")
        return False
    r = subprocess.run(['ffmpeg', '-y', '-i', mp3, '-ar', str(SR), '-ac', '1',
                        '-acodec', 'pcm_s16le', dec],
                       capture_output=True, text=True, timeout=60)

    orig = pcm.astype(np.float64)
    decoded = read_wav(dec)
    corr, snr, delay = best_correlation(orig, decoded)

    # Check ffmpeg decode errors
    errors = check_ffmpeg_errors(mp3)

    passed = abs(corr) >= min_corr and len(errors) == 0
    status = "PASS" if passed else "FAIL"
    print(f"  {status} {name}: corr={corr:+.4f} SNR={snr:.1f}dB delay={delay}"
          f"{' DECODE ERRORS' if errors else ''}")
    return passed


def test_bitrate_range(enc_bin, tmpdir, enc_extra=None):
    """Test that various bitrates encode without errors."""
    t = np.arange(SR) / SR
    pcm = (np.sin(2 * np.pi * 1000 * t) * 20000).astype(np.int16)
    wav = os.path.join(tmpdir, 'br_test.wav')
    gen_wav(wav, pcm)

    passed = True
    for br in [32, 64, 128, 192, 256, 320]:
        mp3 = os.path.join(tmpdir, f'br_{br}.mp3')
        try:
            r = subprocess.run([enc_bin, wav, mp3, '-b', str(br)] + (enc_extra or []),
                               capture_output=True, timeout=30)
            ok = r.returncode == 0 and os.path.getsize(mp3) > 100
            errors = check_ffmpeg_errors(mp3) if ok else ['encode failed']
            ok = ok and len(errors) == 0
        except Exception as e:
            ok = False
        status = "PASS" if ok else "FAIL"
        print(f"  {status} bitrate {br} kbps")
        if not ok:
            passed = False
    return passed


def test_stereo(enc_bin, tmpdir, enc_extra=None):
    """Test stereo encoding."""
    t = np.arange(SR * 2) / SR
    left = (np.sin(2 * np.pi * 440 * t) * 20000).astype(np.int16)
    right = (np.sin(2 * np.pi * 880 * t) * 15000).astype(np.int16)
    stereo = np.column_stack([left, right]).flatten()

    wav = os.path.join(tmpdir, 'stereo.wav')
    mp3 = os.path.join(tmpdir, 'stereo.mp3')

    with wave.open(wav, 'w') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(stereo.astype(np.int16).tobytes())

    r = subprocess.run([enc_bin, wav, mp3, '-b', '128'] + (enc_extra or []),
                       capture_output=True, timeout=30)
    ok = r.returncode == 0 and os.path.getsize(mp3) > 1000
    errors = check_ffmpeg_errors(mp3) if ok else ['encode failed']
    ok = ok and len(errors) == 0
    status = "PASS" if ok else "FAIL"
    print(f"  {status} stereo encode/decode")
    return ok


def test_vbr_range(enc_bin, tmpdir, enc_extra=None):
    """Test that representative VBR qualities encode without decode errors."""
    t = np.arange(SR * 2) / SR
    left = (np.sin(2 * np.pi * 440 * t) * 22000).astype(np.int16)
    right = (np.sin(2 * np.pi * 880 * t) * 18000).astype(np.int16)
    stereo = np.column_stack([left, right]).flatten()

    wav = os.path.join(tmpdir, 'vbr.wav')
    with wave.open(wav, 'w') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(stereo.astype(np.int16).tobytes())

    passed = True
    for q in [0, 5, 9]:
        mp3 = os.path.join(tmpdir, f'vbr_{q}.mp3')
        try:
            r = subprocess.run([enc_bin, wav, mp3, '-V', str(q)] + (enc_extra or []),
                               capture_output=True, timeout=30)
            ok = r.returncode == 0 and os.path.getsize(mp3) > 1000
            errors = check_ffmpeg_errors(mp3) if ok else ['encode failed']
            ok = ok and len(errors) == 0
        except Exception:
            ok = False
        status = "PASS" if ok else "FAIL"
        print(f"  {status} VBR quality {q}")
        if not ok:
            passed = False
    return passed


def main():
    parser = argparse.ArgumentParser(description='glint quality tests')
    parser.add_argument('encoder', nargs='?', default='build/glint_cli',
                        help='Path to glint_cli binary')
    parser.add_argument('--speech', default=None,
                        help='Path to speech WAV file for ASR test')
    parser.add_argument('--fixed', action='store_true',
                        help='Test fixed-point path (-p fixed)')
    args = parser.parse_args()

    enc = args.encoder
    if not os.path.isfile(enc):
        # Try common locations
        for p in ['build/glint_cli', 'build/Release/glint_cli.exe',
                   'build-d/glint_cli', 'build-f/glint_cli', 'build-b/glint_cli']:
            if os.path.isfile(p):
                enc = p
                break
        else:
            print(f"ERROR: encoder not found at {enc}")
            sys.exit(1)

    if args.fixed:
        enc = [enc, '-p', 'fixed']
    else:
        enc = [enc]

    # Wrap encoder path for subprocess
    enc_bin = enc[0]
    enc_extra = enc[1:] if len(enc) > 1 else []

    with tempfile.TemporaryDirectory() as tmpdir:
        all_pass = True
        t5 = np.arange(SR * 5) / SR
        t3 = np.arange(SR * 3) / SR

        # --- Signal quality tests ---
        print("\n=== Signal Quality ===")

        # 1kHz sine (full scale)
        if not test_signal(enc_bin, 'sine_1kHz', np.sin(2*np.pi*1000*t5)*31000, 0.95, tmpdir, enc_extra=enc_extra):
            all_pass = False

        # 440 Hz sine
        if not test_signal(enc_bin, 'sine_440Hz', np.sin(2*np.pi*440*t3)*31000, 0.90, tmpdir, enc_extra=enc_extra):
            all_pass = False

        # Multi-tone (6 frequencies)
        multi = sum(np.sin(2*np.pi*f*t5)*5000 for f in [200, 500, 1000, 2000, 4000, 8000])
        if not test_signal(enc_bin, 'multi_tone', multi, 0.90, tmpdir, enc_extra=enc_extra):
            all_pass = False

        # Speech (if provided)
        if args.speech and os.path.isfile(args.speech):
            speech_pcm = read_wav(args.speech)
            if not test_signal(enc_bin, 'speech', speech_pcm, 0.85, tmpdir, enc_extra=enc_extra):
                all_pass = False

        # --- Bitrate range test ---
        print("\n=== Bitrate Range ===")
        if not test_bitrate_range(enc_bin, tmpdir, enc_extra=enc_extra):
            all_pass = False

        # --- VBR range test ---
        print("\n=== VBR Range ===")
        if not test_vbr_range(enc_bin, tmpdir, enc_extra=enc_extra):
            all_pass = False

        # --- Stereo test ---
        print("\n=== Stereo ===")
        if not test_stereo(enc_bin, tmpdir, enc_extra=enc_extra):
            all_pass = False

        # --- MPEG-II/2.5 sample rate tests ---
        print("\n=== MPEG-II/2.5 Sample Rates ===")
        for sr in [22050, 16000, 11025, 8000]:
            t = np.arange(sr * 2) / sr
            pcm = (np.sin(2 * np.pi * min(400, sr/4) * t) * 20000).astype(np.int16)
            wav = os.path.join(tmpdir, f'sr_{sr}.wav')
            gen_wav(wav, pcm, sr=sr)
            mp3 = os.path.join(tmpdir, f'sr_{sr}.mp3')
            # Use appropriate bitrate for lower sample rates
            br = 64 if sr <= 16000 else 96
            try:
                r = subprocess.run([enc_bin, wav, mp3, '-b', str(br)] + enc_extra,
                                   capture_output=True, text=True, timeout=30)
                ok = r.returncode == 0
                if ok:
                    errors = check_ffmpeg_errors(mp3)
                    ok = len(errors) == 0
            except Exception:
                ok = False
            status = "PASS" if ok else "FAIL"
            print(f"  {status} {sr} Hz @ {br} kbps")
            if not ok:
                all_pass = False

        # --- Transient detection test ---
        print("\n=== Transient Detection ===")
        # Silence then loud burst - should encode without errors
        sr_t = 44100
        pcm_transient = np.zeros(sr_t * 2, dtype=np.int16)
        pcm_transient[sr_t:sr_t+1000] = 30000  # 23ms burst after 1s silence
        wav_t = os.path.join(tmpdir, 'transient.wav')
        gen_wav(wav_t, pcm_transient, sr=sr_t)
        mp3_t = os.path.join(tmpdir, 'transient.mp3')
        try:
            r = subprocess.run([enc_bin, wav_t, mp3_t, '-b', '128'] + enc_extra,
                               capture_output=True, text=True, timeout=30)
            ok_t = r.returncode == 0
            if ok_t:
                errors = check_ffmpeg_errors(mp3_t)
                ok_t = len(errors) == 0
        except Exception:
            ok_t = False
        status = "PASS" if ok_t else "FAIL"
        print(f"  {status} transient signal (silence + burst)")
        if not ok_t:
            all_pass = False

        # --- Summary ---
        print(f"\n{'ALL TESTS PASSED' if all_pass else 'SOME TESTS FAILED'}")
        sys.exit(0 if all_pass else 1)


if __name__ == '__main__':
    main()
