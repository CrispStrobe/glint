# glint

A clean-room MP3 encoder in C++17. MIT licensed. The name nods to
integers (*g-lint*) and the [Shine](https://github.com/toots/shine)
encoder lineage.

Implements the full MPEG-1/2/2.5 Layer III encoding pipeline from the
ISO 11172-3 and ISO 13818-3 standards. No third-party encoder code
referenced. Designed for embedded and real-time use: the fixed-point
path needs only ~46 KB RAM and no FPU.

## Features

- **CBR and VBR** encoding (32-320 kbps, `-V 0-9` quality target)
- **Three signal paths**: double-precision, Q31 fixed-point, or both
  (compile-time or runtime selectable)
- **SIMD**: AVX/SSE2 (x86) and NEON (AArch64), runtime dispatch
  (`-s auto|avx|sse2|neon|none`)
- **Quality tiers** (`-q speed|normal|best`): per-granule scale search that
  matches the source's level and bandwidth, trading encode time for SNR
- **All sample rates**: 8-48 kHz (MPEG-1, MPEG-2, MPEG-2.5)
- **All WAV formats**: PCM 8/16/24/32-bit, IEEE float 32/64, A-law,
  mu-law, WAVE_FORMAT_EXTENSIBLE, raw PCM (`-r`)
- **Streaming API**: callback-based output for real-time use
- **Bindings**: Python (ctypes), Rust (FFI + safe), Dart (Flutter FFI)
- **Embedded**: ~46 KB RAM (fixed-point), fits ESP32/RP2040/STM32F4

## Benchmarks

**Quality** — deterministic speech-like stereo signal, 256 kbps, measured with
`tests/benchmark_encoder.py` and `tests/measure_audio.py`. Both signal paths
(`double` and `fixed`) produce identical output:

| Mode | SNR | seg-SNR | LSD | speed (x86-64) |
|---|---|---|---|---|
| -q speed | 15.1 dB | 15.1 dB | 18.9 dB | ~29× |
| **-q normal** | **14.8 dB** | **14.8 dB** | **18.6 dB** | ~13× |
| -q best | 15.2 dB | 15.2 dB | 18.6 dB | ~5× |

**Per-band SNR vs source** (256 kbps stereo):

| Mode | 0–1 kHz | 1–4 kHz | 4–8 kHz | 8–16 kHz |
|---|---|---|---|---|
| -q speed | 15.7 dB | 4.6 dB | 6.5 dB | 1.3 dB |
| -q normal | 15.3 dB | 5.0 dB | 7.2 dB | 1.4 dB |
| -q best | 15.6 dB | 5.1 dB | 7.3 dB | 1.4 dB |

Mono encoding at 128 kbps reaches ~70× realtime on x86-64 (Intel Xeon,
`-O3 -march=native -ffast-math`, LTO). For a deterministic local speed/quality
run: `python tests/benchmark_encoder.py build/glint_cli`.

**Footprint**:

| | glint double | glint fixed | Shine |
|---|---|---|---|
| Library (.text) | 158 KB | 127 KB | 225 KB |
| Encoder state | 58 KB | 34 KB | — |
| Static tables | 130 KB | 12 KB | — |
| **Total RAM** | **188 KB** | **46 KB** | ~100 KB |
| Process RSS | ~4.5 MB | ~3 MB | — |
| License | **MIT** | **MIT** | LGPL v2 |

Fixed-point mode fits comfortably in ESP32's 520 KB SRAM (<10% usage).
Whisper ASR round-trip: 91% word similarity.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

| Build mode | Flag | RAM (state+tables) |
|---|---|---|
| `double` (default) | — | 188 KB |
| `fixed` | `-DGLINT_MODE=fixed` | 46 KB (no FPU needed) |
| `both` | `-DGLINT_MODE=both` | ~234 KB (runtime `-p` switch) |

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
- **MDCT**: 36-point (long) with fused window×cosine×normalization table
  (eliminates separate windowing loop and per-output /288 division), transposed
  layout for SIMD (12-point short blocks implemented but gated off — see roadmap)
- **Quantization**: double-precision pow34 table with shared precomputation
  across scale factors, anti-clipping gain floor, binary-search gain to the bit
  budget (`quantize_base`), wrapped in a per-granule input-scale search that
  minimizes decoder-reconstruction MSE via `cbrt`-accelerated `granule_mse`
- **Huffman**: O(1) max-value table selection, 34 ISO tables, SCFSI
- **Bitstream**: 32-bit accumulator; each frame self-contained (bit reservoir
  mechanism on the `feature/bit-reservoir` branch, off by default — see roadmap)

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
├── esp-idf/                   ESP32 component (~46 KB RAM)
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

### Recently fixed: level / HF / dulling (per-granule scale search)

`-q speed` and `-q normal` used to lose the top of the spectrum and reconstruct
several dB too quiet (audible as dull, "normalized" sound). Root cause: the
quantizer `is = int(|xr|^0.75 * step + 0.4054)` rounds any coefficient below a
~0.6 dead-zone to zero, so small (mostly high-frequency) coefficients were lost.
A single global gain can't fix this — the right input pre-scale depends on each
granule's spectral shape. `-q best` worked only because it brute-force searched
that scale.

Fixed by unifying all tiers around a **per-granule scale search**
(`quantize_granule` in `src/quantize.cpp`): try several input scale factors, run
the base quantizer (`quantize_base`), and keep the one whose decoder
reconstruction minimizes MSE (`granule_mse`). Search width sets the tier:
speed = 2 factors, normal = 6, best = 12 + a fine refinement. This removed the
old fixed `288/194` gain hack and the (measured no-op) headroom and pruning
passes. Measured on a 1-min 256 kbps stereo speech clip (`double`==`fixed`):

| metric (vs source)   | before (spd/nrm/best) | after (spd/nrm/best) |
|----------------------|-----------------------|----------------------|
| RMS level            | −25.9 / −21.4 / −19.7 | within ~0.2 dB of source, all tiers |
| 95% rolloff          | 1031 / 1031 / 4359 Hz | 3422 / 5344 / 5133 Hz |
| overall SNR          | 5.1 / 10.1 / 15.0 dB  | 12.5 / 14.1 / 14.7 dB |
| encode speed         | —                     | ~29× / 13× / 5× realtime (x86-64 Xeon) |

Verify with `python tests/measure_audio.py original.wav out.mp3` (want RMS
within ~0.5 dB of source, rolloff near source, `double`==`fixed`) and
`ffmpeg -v error -i out.mp3 -f null -` (zero "invalid new backstep").
The `tests/test_quality.py --fixed` path covers signal quality, bitrate range,
stereo, MPEG-2/2.5 sample rates, and transient handling through `-p fixed`.

Remaining headroom: the scale search is still a coarse stand-in for a full
rate-distortion loop. It now includes a decoded spectral-envelope term for
normal/best, but a true per-band bit-allocation loop should improve SNR and
bandwidth together, especially at low bitrate.

### Quality (longer-term)
- **Bark-band psychoacoustic masking** (`-q best`) — zero masked MDCT
  coefficients using spreading function + ATH, target 12-18 dB SNR
  (in progress)
- **Iterative SF amplification** — boost scalefactors for bands exceeding
  masking threshold (builds on psycho model); two approaches tried and failed:
  (1) raw-MSE-gated (`feature/iterative-sf-amplify`): IMDCT overlap-add
  decouples granule MSE from PCM quality, causing regressions; (2) SMR-guided
  (`feature/smr-sf-amplify`): complete no-op because `quantize_base` already
  fills the bit budget — any HF SF boost costs more Huffman bits than are
  available. Real fix requires a full iterative outer loop (à la LAME) that
  trades LF→HF bits by boosting HF scalefactors while increasing global_gain
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
