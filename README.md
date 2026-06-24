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

**Speed** (x-realtime, higher = better):

| Signal | glint double | glint fixed | Shine | LAME |
|---|---|---|---|---|
| Sine | **148x** | 94x | 126x | 48x |
| Complex | 34x | 34x | 98x | 38x |
| Noise | 37x | 35x | 101x | 40x |
| VBR (-V 5) | **240x** | — | — | — |

**Quality** (correlation / SNR):

| Signal | glint | Shine | LAME |
|---|---|---|---|
| Sine | +0.995 / 19 dB | +0.333 / -1 dB | +1.000 / 26 dB |
| Multi-tone | +0.993 / 6 dB | +1.000 / 18 dB | +1.000 / 26 dB |
| Speech | +0.941 / 5 dB | +1.000 / 18 dB | +1.000 / 26 dB |

**Footprint**:

| | glint double | glint fixed | Shine |
|---|---|---|---|
| Library | 158 KB | 127 KB | 225 KB |
| RAM | 141 KB | **50 KB** | ~100 KB |
| License | **MIT** | **MIT** | LGPL v2 |

glint is faster than LAME on simple signals (3x on sine), comparable
on complex signals, and smaller + MIT-licensed vs Shine's LGPL.
Shine has better complex-signal quality; LAME is best overall quality.
glint double and fixed produce identical quality. Whisper ASR
round-trip: 91% word similarity.

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
  -q QUALITY        speed|normal (psychoacoustic model, default: speed)
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

### Quality
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

### Packaging and hardware
- Hardware validation: ESP32 with I2S mic, Cortex-M / RP2040 bare-metal
- Mobile framework packaging: Android AAR, iOS xcframework, Flutter plugin
- Publish packages: pip upload, crates.io publish, vcpkg PR

## License

MIT. See [LICENSE](LICENSE).

All algorithms derived from the public ISO 11172-3 / ISO 13818-3
standards and published academic literature.
