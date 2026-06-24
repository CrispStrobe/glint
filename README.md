# glint

A clean-room MP3 encoder in C++17. MIT licensed. The name nods to
integers (*g-lint*) and the [Shine](https://github.com/toots/shine)
encoder lineage.

Implements the full MPEG-1/2/2.5 Layer III encoding pipeline from the
ISO 11172-3 and ISO 13818-3 standards. No third-party encoder code
referenced. Designed for embedded and real-time use: the fixed-point
path needs only 50 KB RAM and no FPU.

## Features

- **CBR and VBR** encoding (32-320 kbps, `-V 0-9` quality target)
- **Three signal paths**: double-precision, Q31 fixed-point, or both
  (compile-time or runtime selectable)
- **SIMD**: AVX/SSE2 (x86) and NEON (AArch64), runtime dispatch
  (`-s auto|avx|sse2|neon|none`)
- **Psychoacoustic model**: masking-based scalefactor allocation
  (`-q speed|normal`)
- **Short blocks**: transient detection with 12-point MDCT block switching
- **Bit reservoir**: cross-frame bit lending via `main_data_begin`
- **All sample rates**: 8-48 kHz (MPEG-1, MPEG-2, MPEG-2.5)
- **All WAV formats**: PCM 8/16/24/32-bit, IEEE float 32/64, A-law,
  mu-law, WAVE_FORMAT_EXTENSIBLE, raw PCM (`-r`)
- **Streaming API**: callback-based output for real-time use
- **Bindings**: Python (ctypes), Rust (FFI + safe), Dart (Flutter FFI)
- **Embedded**: 50 KB RAM (fixed-point), fits ESP32/RP2040/STM32F4

## Benchmarks vs Shine vs LAME

128 kbps mono, 44100 Hz, Intel Xeon Skylake:

**Quality** (speech SNR, higher = better):

| Mode | glint | Shine | LAME |
|---|---|---|---|
| -q speed | 4.6 dB | — | — |
| **-q normal (default)** | **11.0 dB** | — | — |
| -q best | 12.3 dB | — | — |
| — | — | 18.1 dB | 26.0 dB |

Sine: 18.6 dB (speed), 11.5 dB (normal), 21.8 dB (best).

**Speed** (x-realtime, higher = faster):

| Signal | -q speed | -q normal | -q best | Shine | LAME |
|---|---|---|---|---|---|
| Sine | 204x | 261x | 19x | 190x | 44x |
| Noise | 76x | 56x | 3x | 146x | 37x |
| Speech | 85x | 72x | 4x | — | — |

**Footprint**:

| | glint double | glint fixed | Shine |
|---|---|---|---|
| Library | 158 KB | 127 KB | 225 KB |
| RAM | 141 KB | **50 KB** | ~100 KB |
| License | **MIT** | **MIT** | LGPL v2 |

glint default (-q normal) achieves 11 dB speech SNR at 72x realtime,
MIT licensed, 50 KB RAM. Whisper ASR round-trip: 91% word similarity.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

| Build mode | Flag | RAM footprint |
|---|---|---|
| `double` (default) | — | 141 KB |
| `fixed` | `-DGLINT_MODE=fixed` | 50 KB (no FPU needed) |
| `both` | `-DGLINT_MODE=both` | 191 KB (runtime `-p` switch) |

### Cross-compilation

```bash
# Android (NDK)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DGLINT_MODE=fixed

# iOS
cmake -B build -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=$(xcrun --sdk iphoneos --show-sdk-path) -DGLINT_MODE=fixed

# Raspberry Pi
cmake -B build -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ -DGLINT_MODE=fixed
```

## Usage

### CLI

```
glint_cli [options] input output.mp3
  -b BITRATE        CBR bitrate in kbps (default: 128)
  -V QUALITY        VBR quality 0-9 (0=best, 9=smallest)
  -m MODE           mono|stereo|joint (default: auto)
  -q QUALITY        speed|normal|best (default: normal)
  -s SIMD           auto|avx|sse2|neon|none (default: auto)
  -p PATH           double|fixed (only in both-mode builds)
  -r RATE:CH:BITS   Raw PCM input (e.g., 44100:1:16)
```

### C API

```c
#include <glint/glint.h>

struct glint_config cfg = {
    .sample_rate = 44100, .num_channels = 1,
    .mode = GLINT_MONO, .bitrate = 128,
};

glint_t enc = glint_create(&cfg);

while (have_audio) {
    const int16_t* ch[] = { pcm_buffer };
    int size;
    const uint8_t* mp3 = glint_encode(enc, ch, &size);
    fwrite(mp3, 1, size, output);
}

glint_destroy(enc);
```

| Function | Description |
|---|---|
| `glint_create(cfg)` | Create encoder (CBR or VBR) |
| `glint_create_streaming(cfg, cb, ud)` | Create with frame callback |
| `glint_encode(enc, int16**, &size)` | Encode from int16 |
| `glint_encode_float(enc, float**, &size)` | Encode from float [-1,1] |
| `glint_encode_int32(enc, int32**, &size)` | Encode from int32 |
| `glint_flush(enc, &size)` | Flush final frame |
| `glint_destroy(enc)` | Free encoder |

### Python

```python
import glint
glint.encode_file("input.wav", "output.mp3", bitrate=128)

with glint.Encoder(44100, 1, 128) as enc:
    mp3 = enc.encode(pcm_int16) + enc.flush()
```

### Rust

```rust
let mut enc = glint::Encoder::new(44100, 1, 128)?;
let mp3 = glint::encode_pcm(&samples, 44100, 1, 128);
```

## Architecture

```
PCM input → Subband Analysis → MDCT → Alias Reduction → Quantization
    → Huffman Encoding → Bitstream Assembly → MP3 frame
```

- **Subband**: 512-point polyphase filter bank (AVX/SSE2/NEON vectorized)
- **MDCT**: 36-point (long) or 12-point (short) with sine window, /288
  normalization, transposed cosine table for SIMD
- **Quantization**: pow34 table lookup, anti-clipping gain floor, binary
  search (CBR) or target gain (VBR), masking-based scalefactors
- **Huffman**: O(1) max-value table selection, 34 ISO tables, SCFSI
- **Bitstream**: 32-bit accumulator, bit reservoir with `main_data_begin`

## Project structure

```
glint/
├── include/glint/glint.h      C API
├── src/                       Encoder core (7 modules + tables)
├── cli/main.cpp               WAV-to-MP3 CLI
├── tests/                     Unit tests + quality tests + ASR
├── bindings/
│   ├── python/                ctypes wrapper + pip packaging
│   ├── rust/                  glint-sys (FFI) + glint (safe)
│   └── dart/                  Flutter FFI
├── esp-idf/                   ESP32 component (50 KB RAM)
├── packaging/vcpkg/           vcpkg port
├── .github/workflows/         CI + release (9 platforms)
└── CMakeLists.txt
```

## CI and releases

Builds and tests on 9 platforms: Linux x86-64, Linux aarch64, macOS
Apple Silicon, Windows MSVC, Android arm64-v8a/armeabi-v7a/x86_64,
iOS arm64 device + simulator.

Pre-built binaries at
[github.com/CrispStrobe/glint/releases](https://github.com/CrispStrobe/glint/releases).

## Roadmap

### Diagnosed quality issues (priority — start here)

These were measured on a 1-minute 256 kbps stereo speech clip with
`tests/measure_audio.py REF FILE...` (decodes via ffmpeg, reports dynamics,
spectral shape, band SNR, and where quantization noise sits). The `double`
and `fixed` paths are bit-identical on every metric, so these are encoder
(quantizer/MDCT) issues, not path issues. Observed vs. the original:

| metric          | original | `-q speed` | `-q normal` | `-q best` |
|-----------------|----------|------------|-------------|-----------|
| RMS level (dB)  | −19.5    | −25.9      | −21.4       | −19.7     |
| 95% spectral rolloff | 5414 Hz | **1031 Hz** | **1031 Hz** | 4359 Hz |
| overall SNR     | —        | 5.1 dB     | 10.1 dB     | 15.0 dB   |
| seg-SNR         | —        | 3.4 dB     | 6.8 dB      | 13.4 dB   |

All three issues below share one root cause; fix #1 first, the others should
largely fall out of it.

**1. MDCT gain / normalization mismatch (root cause).**
The forward MDCT divides every coefficient by 288 (`src/mdct.cpp` long-block
paths ~lines 88/102/114/125, fixed-point 252/261, float 338/350/364; 12-point
short uses /96 at 187). That leaves coefficients at a scale where the quantizer
rounds small ones to zero: `quantize_and_count()` computes
`qval = pow34(|xr|)*base_step*sf + 0.4054` and truncates to int
(`src/quantize.cpp:81,97`), with `base_step = 2^(-3*(global_gain-210)/16)`. With
the /288 scale the natural `global_gain` lands too coarse, so anything but the
loudest (low-frequency) coefficients truncate to 0.
The codebase currently *papers over* this with two hacks:
 - `-q normal`: a fixed `kGainCorrection = 288.0/194.0 ≈ 1.485` multiply of the
   MDCT input (`src/quantize.cpp:316-325`).
 - `-q best`: a 0.70–2.20 *factor search* (`src/quantize.cpp:242-275`) that
   picks the multiplier minimizing decoder-reconstruction MSE
   (`mse_vs_original`, `src/quantize.cpp:213-232`). This is why `best` works and
   matches the original level (−19.7 vs −19.5 dB) — it effectively searches for
   the missing normalization. `speed` applies no correction (hence −25.9 dB,
   6.4 dB quiet) and `normal`'s fixed 1.485 is not enough.
 *Fix:* make the forward MDCT scale so the standard decode
 (`xr = ix^(4/3) * 2^(0.25*(global_gain-210)) * sf_dec`) reconstructs the input
 1:1 at a mid-range `global_gain`, then delete `kGainCorrection` and re-center
 the `-q best` factor search tightly around 1.0. Derive the constant from a
 sine round-trip (encode → decode formula → compare magnitude) rather than
 guessing; the `mse_vs_original` lambda already encodes the exact decode math to
 check against. Verify the optimal `global_gain` is no longer pinned near
 `min_gain` (the anti-clip floor at `src/quantize.cpp:332-353`).

**2. High-frequency hard-cut at ~1 kHz (`-q speed`, `-q normal`).**
Both modes throw away nearly everything above ~1 kHz (95% rolloff 1031 Hz vs
the source's 5414 Hz) — audible as dull / "normalized" sound. Two contributors:
(a) the gain mismatch in #1 (coarse gain ⇒ small HF coefficients round to 0),
and (b) `compute_headroom_scalefactors()` (`src/quantize.cpp:154-199`) assigns
`sf = 0` to HF bands it judges masked or "far above mask," removing the
precision those bands need to survive quantization. `-q best` avoids this
because its MSE-driven factor/prune search keeps HF (rolloff 4359 Hz).
 *Fix:* after #1, re-check rolloff; then stop the headroom model from zeroing
 bands that carry real energy (floor `sf` for audible HF bands, or gate the
 masking decision on absolute band energy / ATH instead of relative SMR).
 Target: rolloff within ~1 kHz of the source at 256 kbps.

**3. Coarse quantization / dynamics flattening (`-q speed`, `-q normal`).**
At 256 kbps the frames are *not* bit-limited (the bit reservoir is transparent
there), yet `speed`/`normal` quantize coarsely: noise concentrates below 1 kHz
(78% of error power for `speed`) and short-term dynamic range inflates (DR 38 dB
vs the source's 27 dB) because coarse steps deepen quiet passages. The binary
gain search (`src/quantize.cpp:360-370`) already seeks the finest gain that fits
the budget, so spare bits exist but aren't used — likely because `min_gain`
(anti-clip) or the headroom `sf` assignment prevents going finer.
 *Fix:* falls out of #1 + #2 (correct scale ⇒ finer natural gain ⇒ spare bits
 spent on precision). If a gap remains, add the existing roadmap item
 *Iterative SF amplification* to spend leftover bits on the worst bands.

**How to verify any change here**
 - Quality: `python tests/measure_audio.py original.wav out_*.mp3` — want RMS
   within ~0.5 dB of source, rolloff near source, seg-SNR up for speed/normal,
   and `double`==`fixed` parity preserved.
 - Correctness (do not regress): `ffmpeg -v error -i out.mp3 -f null -` must
   report **zero** "invalid new backstep"; `ctest` / `glint_test` must pass.
 - Note `-q best`'s factor/prune search is slow (~6× realtime); once #1 lands,
   much of it can be removed, which also speeds `best` up.

### Quality (longer-term)
- **Bark-band psychoacoustic masking** (`-q best`) — zero masked MDCT
  coefficients using spreading function + ATH, target 12-18 dB SNR
  (in progress)
- **Iterative SF amplification** — boost scalefactors for bands exceeding
  masking threshold (builds on psycho model)
- **Temporal noise shaping (TNS)** — filter quantization noise to follow
  signal envelope, reduce pre-echo without short blocks
- **Mixed blocks** — short blocks for low subbands (pre-echo sensitive),
  long blocks for the rest
- **Perceptual entropy** — estimate per-granule bit need from masking,
  use for VBR quality targeting and block switching decisions
- **Inter-granule bit allocation** — give more bits to high-energy
  granules instead of equal split
- **Bit reservoir allocation policy** — the reservoir *mechanism* is
  implemented and correct on the `feature/bit-reservoir` branch (0 backstep,
  transparent with conservative budgeting) but not merged: a naive
  spend-everything policy regresses low-bitrate quality. Needs perceptual rate
  control (save bits on easy frames, borrow for hard ones, cap per-frame
  borrowing). See that branch's `available_bits` comment.
- **Short blocks** — currently disabled (`kShortBlocksEnabled=false` in
  `src/encoder.cpp`): they were starved/broken without the reservoir and hurt
  quality 4–9 dB. Re-enable once the reservoir lands and the short-block path is
  validated.

### Packaging and hardware
- Hardware validation: ESP32 with I2S mic, Cortex-M / RP2040 bare-metal
- Mobile framework packaging: Android AAR, iOS xcframework, Flutter plugin
- Publish packages: pip upload, crates.io publish, vcpkg PR

## License

MIT. See [LICENSE](LICENSE).

All algorithms derived from the public ISO 11172-3 / ISO 13818-3
standards and published academic literature.
