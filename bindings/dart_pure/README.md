# glint_audio_pure

A **pure-Dart MP3 (MPEG-1 Layer III) encoder** — no native code, no FFI, no
runtime dependencies. It runs identically on every platform Dart targets,
**including the web**, where FFI-backed codecs can't go.

It is the pure-Dart sibling of [`glint_audio`](https://pub.dev/packages/glint_audio)
(FFI bindings to the [glint](https://github.com/CrispStrobe/glint) C++ codec
suite). Use `glint_audio` when you want the full suite (MP3/AAC/Opus/WAV +
resampling) at native speed; use **`glint_audio_pure`** when you need MP3
encoding that just works everywhere with zero setup.

```dart
import 'dart:typed_data';
import 'package:glint_audio_pure/glint_audio_pure.dart';

// pcm: mono samples in [-1, 1]
final Uint8List mp3 = mp3EncodeMono(pcm, sampleRate: 44100, bitrate: 128);
// -> a standard .mp3 any decoder plays.
```

## Why

- **All platforms, including web.** Pure `dart:core` / `dart:math` /
  `dart:typed_data`. No `dart:ffi`, no bundled binaries, no build hooks.
- **Standard output.** Produces conformant MPEG-1 Layer III that ffmpeg, browser
  `<audio>`, and every other decoder play.
- **Faithful port.** Clean-room port of glint's encoder. The DSP front-end is
  machine-equivalent to the C++ reference (subband 5e-15, MDCT 7e-16 relative
  error) and the quantizer reproduces glint's per-granule decisions.

## What it does

The full MPEG-1 Layer III encode pipeline:

1. 32-band polyphase (subband) analysis
2. MDCT — long blocks (and opt-in short/transient blocks), with alias reduction
   and frequency inversion
3. Rate/distortion quantization loop with a **psychoacoustic masking model**:
   scalefactors are amplified to push quantization noise under the Bark-band
   masking threshold (Schroeder spreading + ATH), with `scalefac_scale` and
   `preflag`
4. Huffman coding (the ISO tables + count1 quads)
5. Constant-bitrate frame assembly (header + side info + main data)

## Scope & quality

- **Now:** encode mono / stereo / joint(M/S), CBR + VBR (Xing header), long
  blocks plus opt-in short/transient blocks (`shortBlocks: true`, mono AND
  stereo/joint); DECODE every block type (long/short/mixed/start/stop) — the
  codec round-trips in pure Dart and decodes any real-world MP3.
- Benchmarked against glint's own `measure_audio.py` on a speech signal at
  128 kbps mono: **SNR 35.2 dB** (glint 32.1 dB), band 0–1 kHz **40.6 dB**
  (glint 36.3). Raw SNR exceeds the reference; perceptual noise-to-mask (NMR)
  is a touch behind (a Huffman region-optimizer refinement — the next quality
  step). A 200→3000 Hz sweep decodes at 78 dB.
- **Not yet:** AAC/Opus. Follow the repo for progress.

## API

- `Uint8List mp3EncodeMono(Float64List pcm, {int sampleRate = 44100, int bitrate = 128})`
  — encode mono PCM in [-1, 1] to a CBR `.mp3`. `sampleRate` ∈ {44100, 48000,
  32000}; `bitrate` is any MPEG-1 rate (32…320 kbps).
- `mp3EncodeStereo` / `mp3EncodeJointStereo(left, right, ...)` — stereo & mid/side.
- `mp3EncodeMonoVbr` / `mp3EncodeStereoVbr(..., quality: 0..9)` — variable bitrate.
- `Mp3Pcm mp3Decode(Uint8List mp3)` — decode back to interleaved float PCM.
- WAV helpers: `readWavPcm16(bytes) → WavData`, `wavToMonoFloat(WavData) → Float64List`.
- Frame constants: `kMp3Bitrates`, `kMp3SampleRates`, `mp3FrameSize(...)`.

## License

MIT. Ported from [glint](https://github.com/CrispStrobe/glint) (MIT).
