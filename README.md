# glint

A clean-room MP3 encoder in C++17 -- glint, as in g-lint (integers), a nod to the [Shine](https://github.com/toots/shine) encoder lineage. MIT licensed.

Implements the full MPEG-1/2/2.5 Layer III encoding pipeline from the ISO
11172-3 and ISO 13818-3 standards without referencing any existing encoder
source code. No psychoacoustic model -- uses energy-based scalefactor
allocation for a lightweight, predictable encoder suitable for embedded and
real-time use.

## Quality

At 128 kbps mono, 44100 Hz (measured against ffmpeg decode):

| Signal | Correlation | SNR |
|---|---|---|
| 1 kHz sine | 0.9955 | 18.6 dB |
| 440 Hz sine | 0.9900 | 16.1 dB |
| Multi-tone (6 freq) | 0.9930 | 5.9 dB |
| Speech (JFK) | 0.9408 | 4.6 dB |

Whisper ASR round-trip: 91% word similarity (identical content,
punctuation differences only). All bitrates 32-320 kbps decode
without errors via ffmpeg.

## Speed

Encoding speed on Intel Xeon Skylake, single-threaded, Release build:

| Signal | Mono 128 kbps | vs LAME |
|---|---|---|
| Sine | 144x realtime | 3.6x faster |
| Speech | ~80x realtime | 2x faster |
| Noise | 38x realtime | ~1x (comparable) |

Optimizations: pow34 table lookup, fused subband+MDCT pipeline,
max-value Huffman table selection, pragma loop unrolling,
`-march=native -ffast-math`. Bit reservoir enabled for cross-frame
bit lending.

## Building

Requires CMake >= 3.16 and a C++17 compiler.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

Build modes (`-DGLINT_MODE=`):

| Mode | Flag | Description |
|---|---|---|
| `double` | (default) | Double-precision signal path |
| `fixed` | `-DGLINT_MODE=fixed` | Q31 fixed-point (no FPU needed) |
| `both` | `-DGLINT_MODE=both` | Both paths, runtime `-p` flag |

### Cross-compilation

```bash
# Android (NDK)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DGLINT_MODE=fixed

# iOS
cmake -B build -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DCMAKE_OSX_SYSROOT=$(xcrun --sdk iphoneos --show-sdk-path) -DGLINT_MODE=fixed

# Raspberry Pi (cross-compile from x86)
cmake -B build -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ -DGLINT_MODE=fixed

# ESP32 (ESP-IDF component — add to your project's components/)
# Uses fixed-point path, no FPU required
```

Produces:
- `libglint.a` / `libglint.so` -- static and shared libraries
- `glint_cli` -- command-line encoder (desktop only)

## Usage

### CLI

```
glint_cli [options] input.wav output.mp3
  -b BITRATE        Bitrate in kbps (default: 128)
  -m MODE           mono|stereo|joint (default: auto)
  -s SIMD           auto|avx|sse2|none (default: auto)
  -r RATE:CH:BITS   Raw PCM input (e.g., 44100:1:16)
```

Input formats: WAV with PCM (8/16/24/32-bit), IEEE float (32/64-bit),
A-law, mu-law, or WAVE_FORMAT_EXTENSIBLE. Raw headerless PCM via `-r`.
Sample rates: 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 Hz.

### C API

```c
#include <glint/glint.h>

struct glint_config cfg = {
    .sample_rate  = 44100,
    .num_channels = 1,
    .mode         = GLINT_MONO,
    .bitrate      = 128,
};

glint_t enc = glint_create(&cfg);
int frame_samples = glint_samples_per_frame(enc);  // 1152 for MPEG-1

while (have_audio) {
    const int16_t* channels[] = { pcm_buffer };
    int out_size;
    const uint8_t* mp3 = glint_encode(enc, channels, &out_size);
    fwrite(mp3, 1, out_size, output_file);
}

glint_destroy(enc);
```

| Function | Description |
|---|---|
| `glint_check_config(sr, br)` | Validate sample rate / bitrate pair |
| `glint_create(cfg)` | Create encoder, returns handle (NULL on error) |
| `glint_samples_per_frame(enc)` | Samples per channel per frame (1152 or 576) |
| `glint_encode(enc, channels, &size)` | Encode from `int16_t**` |
| `glint_encode_float(enc, channels, &size)` | Encode from `float**` (range [-1, 1]) |
| `glint_encode_int32(enc, channels, &size)` | Encode from `int32_t**` (full 32-bit range) |
| `glint_flush(enc, &size)` | Flush remaining data |
| `glint_destroy(enc)` | Free encoder |

Thread safety: independent handles are fully independent; a single handle
must not be used concurrently.

## Architecture

```
PCM int16 input
  |
  v
+---------------------+
|  Subband Analysis    |  Polyphase filter bank: 512-point window,
|                      |  32-band cosine matrixing (ISO Table C.1)
+--------+------------+
         |  32 subbands x 36 time slots
         v
+---------------------+
|  MDCT               |  36->18 modified discrete cosine transform
|                      |  with sine window, overlap-add (TDAC)
+--------+------------+
         |  576 frequency coefficients per granule
         v
+---------------------+
|  Alias Reduction     |  8-point butterfly at subband boundaries
|                      |  (ISO Table B.9 coefficients)
+--------+------------+
         |
         v
+---------------------+
|  Quantization        |  Binary search for global_gain, energy-based
|                      |  scalefactor allocation, ix = floor(|xr|^3/4 / step)
+--------+------------+
         |  576 quantized integers + gain + scalefactors
         v
+---------------------+
|  Huffman Encoding    |  ISO Table B.7 (32 pair tables + 2 quad tables),
|                      |  3-region partitioning, ESC + linbits
+--------+------------+
         |
         v
+---------------------+
|  Bitstream Assembly  |  Frame header (32b) + side info (136/256b)
|                      |  + main data, MSB-first big-endian
+---------------------+
         |
         v
     MP3 frame bytes
```

### Key implementation details

**Subband analysis** -- 512-sample circular buffer with ISO 11172-3 Table C.1
window coefficients. Samples are stored in reverse order within each
32-sample block to match the standard's FIFO convention (newest at index 0).
Frequency inversion (sign flip of odd time-slots in odd subbands) is applied
before the MDCT.

**MDCT** -- Forward transform with /288 normalization factor
(= 32 subband matrixing gain x 9 MDCT half-length) to compensate the
round-trip gain of the analysis->MDCT->IMDCT->synthesis chain. Sine window
for time-domain alias cancellation (TDAC).

**Quantization** -- The inner loop uses `ix = floor(|xr|^(3/4) * gain + 0.4054)`
with anti-clipping: computes the minimum `global_gain` that keeps all
quantized values below 8191 before starting the binary search. `x^(3/4)` uses
interpolating lookup in a 10000-entry table (no `std::pow` in the hot loop).
Energy-based scalefactor assignment with spectral diversity guard. SCFSI
shares identical scalefactors between granules. Bit reservoir enables
cross-frame bit lending via `main_data_begin`.

**Huffman tables** -- All 34 tables from ISO 11172-3 Annex B (Tables B.7).
Region boundaries derived from scalefactor band tables. Max-value-indexed
table selection (O(1) lookup). ESC tables (16-31) with linbits for
values >= 15.

**MPEG version support** -- MPEG-1 (32/44.1/48 kHz, 2 granules),
MPEG-2 (16/22.05/24 kHz, 1 granule), MPEG-2.5 (8/11.025/12 kHz, 1 granule).
Bitrate tables and scalefactor band boundaries per ISO 13818-3.

## Project structure

```
glint/
+-- include/glint/glint.h   C API header
+-- src/
|   +-- encoder.cpp/.hpp      Orchestrator, frame assembly, API implementation
|   +-- subband.cpp/.hpp      Polyphase analysis filter bank
|   +-- mdct.cpp/.hpp         MDCT + alias reduction
|   +-- quantize.cpp/.hpp     Quantization loop + scalefactor allocation
|   +-- huffman.cpp/.hpp      Huffman encoding + region partitioning
|   +-- reservoir.cpp/.hpp    Bit reservoir (interface present, currently disabled)
|   +-- bitstream.hpp         Bit-level writer + frame assembler
|   +-- fixedpoint.hpp        Q31 arithmetic primitives (ARM/MIPS/portable)
|   +-- tables.hpp            All ISO constant data (window, Huffman, SFB, pow34)
+-- cli/main.cpp              WAV-to-MP3 command-line tool
+-- CMakeLists.txt
```

## Roadmap

### Done

- **Fixed-point signal path** -- Q24/Q31 integer subband analysis,
  MDCT, and alias reduction via `-DGLINT_MODE=fixed`. Three build
  modes: double, fixed, both (runtime `-p` flag). Platform-portable
  (`__int128` on GCC/Clang, int64 fallback on MSVC).
- **pow34 table lookup** -- interpolating `pow34_table[10000]` replaces
  `std::pow(x, 0.75)` in the quantizer (2.09x speedup).
- **Fused subband+MDCT pipeline** -- eliminated 72 KB intermediate buffer.
- **Huffman max-value lookup** -- O(1) table selection replaces
  exhaustive trial (+10-37% speedup).
- **Loop unrolling** -- `#pragma GCC unroll` on all hot loops.
- **Bit reservoir** -- cross-frame bit lending via `main_data_begin`.
- **CI/CD** -- GitHub Actions on Linux x86-64/aarch64, macOS, Windows.
  Release workflow for tagged versions.
- **Test suite** -- C++ unit tests, Python quality/ASR tests.

### Planned

- **ARM NEON** -- SIMD intrinsics for ARM (subband + MDCT)
- **Quality** -- psychoacoustic energy model, MPEG-II/2.5 validation,
  short block support for transients
- **Mobile** -- Android AAR / iOS framework packaging, Flutter plugin
- **Embedded** -- ESP-IDF component, Raspberry Pi Pico (RP2040)
  validation with fixed-point path
- **Distribution** -- vcpkg, Conan, pip (`glint-mp3`), crates.io
- **Language bindings** -- Rust (`glint-sys` + safe crate), Dart (FFI
  for Flutter), Python (ctypes)

## License

MIT. See [LICENSE](LICENSE).

All algorithms derived from the public ISO 11172-3 / ISO 13818-3 standards
and published academic literature. No third-party encoder code referenced.

## References

- ISO/IEC 11172-3:1993 -- MPEG-1 Audio (Layer III)
- ISO/IEC 13818-3:1998 -- MPEG-2 Audio
- Davis Pan, "A Tutorial on MPEG/Audio Compression", IEEE Multimedia, 1995
