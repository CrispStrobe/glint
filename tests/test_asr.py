#!/usr/bin/env python3
"""
glint ASR round-trip test.

Encodes speech with glint, decodes with ffmpeg, runs Whisper ASR on both
original and decoded, compares transcriptions.

Requires: ffmpeg, glint_cli, whisper (pip install openai-whisper).

Usage:
    python3 tests/test_asr.py [path/to/glint_cli] [path/to/speech.wav]
"""

import subprocess
import sys
import os
import tempfile

SR = 44100


def main():
    enc = sys.argv[1] if len(sys.argv) > 1 else 'build/glint_cli'
    speech = sys.argv[2] if len(sys.argv) > 2 else None

    # Find speech file
    if speech is None:
        for p in ['/mnt/volume1/tmp-overflow/jfk_44k.wav', 'tests/jfk_44k.wav']:
            if os.path.isfile(p):
                speech = p
                break
    if speech is None or not os.path.isfile(speech):
        print("SKIP: no speech file found (pass path as second argument)")
        sys.exit(0)

    if not os.path.isfile(enc):
        for p in ['build/glint_cli', 'build/Release/glint_cli.exe']:
            if os.path.isfile(p):
                enc = p
                break

    # Check whisper is available
    try:
        import whisper
    except ImportError:
        print("SKIP: whisper not installed (pip install openai-whisper)")
        sys.exit(0)

    with tempfile.TemporaryDirectory() as tmpdir:
        mp3 = os.path.join(tmpdir, 'speech.mp3')
        dec = os.path.join(tmpdir, 'speech_dec.wav')
        orig_16k = os.path.join(tmpdir, 'orig_16k.wav')

        # Encode
        r = subprocess.run([enc, speech, mp3, '-b', '128'],
                           capture_output=True, timeout=60)
        if r.returncode != 0:
            print(f"FAIL: encode failed")
            sys.exit(1)

        # Decode to 16kHz for whisper
        subprocess.run(['ffmpeg', '-y', '-i', mp3, '-ar', '16000', '-ac', '1',
                        '-acodec', 'pcm_s16le', dec],
                       capture_output=True, timeout=30)
        subprocess.run(['ffmpeg', '-y', '-i', speech, '-ar', '16000', '-ac', '1',
                        '-acodec', 'pcm_s16le', orig_16k],
                       capture_output=True, timeout=30)

        # Transcribe both
        print("Loading Whisper model...")
        model = whisper.load_model("base")

        print("Transcribing original...")
        r_orig = model.transcribe(orig_16k, language='en')
        orig_text = r_orig['text'].strip()

        print("Transcribing encoded→decoded...")
        r_dec = model.transcribe(dec, language='en')
        dec_text = r_dec['text'].strip()

        # Compare
        orig_words = orig_text.lower().split()
        dec_words = dec_text.lower().split()

        from difflib import SequenceMatcher
        similarity = SequenceMatcher(None, orig_words, dec_words).ratio()

        print(f"\nOriginal:  {orig_text}")
        print(f"Decoded:   {dec_text}")
        print(f"Words: {len(orig_words)} orig, {len(dec_words)} decoded")
        print(f"Similarity: {similarity:.1%}")

        # Pass if word similarity >= 90%
        passed = similarity >= 0.90
        print(f"\n{'PASS' if passed else 'FAIL'}: ASR round-trip "
              f"({'identical' if orig_words == dec_words else f'{similarity:.0%} similar'})")
        sys.exit(0 if passed else 1)


if __name__ == '__main__':
    main()
