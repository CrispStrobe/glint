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
- **Not yet:** AAC/Opus. Follow the repo for progress.

## Benchmark vs LAME / ffmpeg

Encode → decode (ffmpeg) → best-lag/gain-aligned SNR against the source, on
three real 4 s / 44.1 kHz mono clips: a rendered instrumental band
(drums+bass+chords+melody), macOS `say` speech, and an isolated bell attack.
CBR, so file sizes are fixed by bitrate (ours runs a few hundred bytes smaller —
no ID3/padding).

**Reconstruction SNR (dB, higher = closer to source):**

| clip | kbps | ours | LAME | ffmpeg |
|------|-----:|-----:|-----:|-------:|
| band (drums+bass+chords+melody) | 128 | **33.8** | 31.6 | 31.6 |
|  | 192 | **42.5** | 34.5 | 34.5 |
| speech | 128 | **44.8** | 42.2 | 42.2 |
|  | 192 | **55.8** | 53.3 | 53.3 |
| bell attack | 128 | **73.8** | 73.6 | 73.6 |
|  | 192 | **78.3** | 77.0 | 77.0 |

Our encoder leads on raw SNR at every point — it spreads quantization noise more
evenly. LAME/ffmpeg trade raw SNR for perceptual placement (noise pushed under
the masking threshold), so a lower SNR there is partly by design; NMR
(noise-to-mask) is the metric where the reference still edges ahead, and closing
it is the next quality step.

> Short/transient blocks help where pre-echo is actually audible — an isolated
> attack after quiet (the bell gains up to +0.9 dB in the worst 20 ms window).
> The conservative trigger deliberately leaves dense mixes on long blocks, and
> that's correct: measured across bitrates, forcing short blocks onto the *band*
> clip *costs* ~0.5 dB (the window-switching quantizer is not psy-shaped yet, so
> the extra short-block bits don't pay off), and inside a busy mix the ongoing
> signal forward-masks pre-echo anyway. So the real quality lever is
> **psychoacoustic shaping of the short-block quantizer** (short scalefactors),
> not a more eager detector — making the short blocks that already fire more
> bit-efficient, and closing the NMR gap generally.

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
