# glint

A clean-room MP3 encoder in C++17 -- glint, as in g-lint (integers), a nod to the [Shine](https://github.com/toots/shine) encoder lineage. MIT licensed.

Implements the full MPEG-1/2/2.5 Layer III encoding pipeline from the ISO
11172-3 and ISO 13818-3 standards without referencing any existing encoder
source code. No psychoacoustic model -- uses energy-based scalefactor
allocation for a lightweight, predictable encoder suitable for embedded and
real-time use.

## Quality

At 128 kbps mono, 44100 Hz (measured against ffmpeg decode):

| Signal | Correlation | SNR | Amplitude ratio |
|---|---|---|---|
| 1 kHz sine | 1.0000 | 81.5 dB | 1.000 |
| Speech (JFK) | 1.0000 | 57.3 dB | 1.000 |
| Multi-tone (6 freq) | 1.0000 | 55.1 dB | 1.000 |

Whisper ASR produces identical word-level transcription on the original
and encoded-then-decoded speech.

## Speed

Encoding speed on Intel Xeon Skylake, single-threaded, `-O2`:

| Mode | Bitrate | Speed |
|---|---|---|
| Mono | 128 kbps | 7.1x realtime |
| Mono | 320 kbps | 8.0x realtime |
| Stereo | 128 kbps | 6.3x realtime |
| Stereo | 320 kbps | 3.5x realtime |

For comparison, LAME achieves ~40x mono / ~20x stereo on the same hardware.
The gap is primarily due to the double-precision signal path (LAME uses
optimized integer/SIMD); converting to fixed-point (Phase 1 in the roadmap)
is expected to close this significantly.

## Building

Requires CMake >= 3.16 and a C++17 compiler.

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Produces:
- `libglint.a` / `libglint.so` -- static and shared libraries
- `glint_cli` -- command-line encoder

## Usage

### CLI

```
glint_cli [options] input.wav output.mp3
  -b BITRATE   Bitrate in kbps (default: 128)
  -m MODE      mono|stereo|joint (default: auto)
```

Input must be 16-bit PCM WAV. Supported sample rates: 8000, 11025, 12000,
16000, 22050, 24000, 32000, 44100, 48000 Hz.

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
| `glint_encode(enc, channels, &size)` | Encode one frame, returns MP3 bytes |
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
quantized values below 8191 before starting the binary search. Energy-based
scalefactor assignment with spectral diversity guard (only activates for
broadband signals). SCFSI shares identical scalefactors between granules.

**Huffman tables** -- All 34 tables from ISO 11172-3 Annex B (Tables B.7).
Region boundaries derived from scalefactor band tables. Table selection
by exhaustive trial per region. ESC tables (16-31) with linbits for
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

The encoder is functional and produces correct MP3 output. Planned
improvements grouped by phase:

### Phase 1 -- Fixed-point signal path

Convert the per-frame hot path from `double` to `int32_t` Q31, gated by
a compile-time flag (`-DGLINT_FIXED_POINT`) so both paths coexist.
A third build mode (`-DGLINT_BOTH`) compiles both and selects at
runtime via CLI flag (`-p fixed`/`-p double`) or API parameter.

- Convert subband analysis, MDCT, alias reduction to Q31 using
  `fpmul()`/`fpmul_acc()` from `fixedpoint.hpp`
- Activate platform multiply intrinsics: ARM `smmul`/`smmla`,
  MIPS `mult`/`mfhi` (already defined in `fixedpoint.hpp`, unused)
- Integer-indexed `pow34_table[10000]` -- replace `std::pow(x, 0.75)`
  with direct lookup in the quantizer inner loop
- Precomputed gain step table for the quantizer binary search
- Target: >= 200x realtime on modern x86-64

### Phase 2 -- Pipeline optimisation

- **Fused subband+MDCT** -- eliminate the intermediate 32x36 buffer
  by streaming subband samples directly into the MDCT (~20-30%
  speedup on cache-constrained cores)
- **Loop unrolling** -- manual unroll of subband (64 MACs), MDCT
  (36 MACs), and alias reduction (8 butterflies) by factor 4-8
- **Huffman table selection** -- max-value-indexed lookup instead of
  exhaustive trial per region

### Phase 3 -- Bit reservoir

- Enable cross-frame bit lending via `main_data_begin` (interface
  exists in `reservoir.hpp`, disabled pending frame assembly work)
- Back-buffer for main data spanning across frames
- Stuffing bits to drain reservoir at end-of-stream
- Expected quality gain: 1-2 dB SNR on variable-complexity material

### Phase 4 -- SIMD

- x86 SSE/AVX for subband matrixing (32x64 matrix-vector multiply
  is a natural fit for `_mm_madd_epi16` / `_mm256_mullo_epi32`)
- x86 SSE/AVX for MDCT inner loop (36-point dot product)
- NEON intrinsics for ARM (complementing the existing scalar
  `smmul`/`smmla` paths)

### Phase 5 -- Quality refinements

- Simplified psychoacoustic energy model for smarter per-band bit
  allocation (energy-weighted scalefactors instead of uniform)
- Full MPEG-II/2.5 validation across all sample rates
- Short block support for transient signals

### Phase 6 -- CI/CD and distribution

- GitHub Actions: build + test on Linux (x86-64, aarch64), macOS
  (Apple Silicon), Windows (MSVC)
- Release binaries: pre-built CLI for all platforms via CI
- Package manager support (vcpkg, Conan)

### Phase 7 -- Language bindings

- **Rust** — `glint-sys` (raw FFI) + `glint` (safe wrapper crate)
- **Dart** — FFI bindings for Flutter mobile/desktop MP3 encoding
- **Python** — `ctypes` or `pybind11` wrapper

## License

MIT. See [LICENSE](LICENSE).

All algorithms derived from the public ISO 11172-3 / ISO 13818-3 standards
and published academic literature. No third-party encoder code referenced.

## References

- ISO/IEC 11172-3:1993 -- MPEG-1 Audio (Layer III)
- ISO/IEC 13818-3:1998 -- MPEG-2 Audio
- Davis Pan, "A Tutorial on MPEG/Audio Compression", IEEE Multimedia, 1995
