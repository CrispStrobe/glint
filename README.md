# glint

A clean-room MP3 and AAC-LC encoder in C++17. MIT licensed. The name
nods to integers (*g-lint*) and the [Shine](https://github.com/toots/shine)
encoder lineage.

Implements the full MPEG-1/2/2.5 Layer III encoding pipeline from the
ISO 11172-3 and ISO 13818-3 standards, plus an AAC-LC encoder (ISO
13818-7 / 14496-3, ADTS output). No third-party encoder code
referenced (the normative AAC Huffman/scalefactor-band tables are ISO
spec data, extracted from two independent implementations and
cross-checked bit-for-bit — see `tools/gen_aac_tables.py`). Designed
for embedded and real-time use: the MP3 fixed-point build needs ~64 KB
RAM (Q31 filterbank; the rate loop's scalars run fine via soft-float),
and the AAC build needs 47.6 KB with a fully integer per-coefficient
hot path for FPU-less parts (RP2040-class).

## Features

- **CBR and VBR** encoding (32-320 kbps, `-V 0-9` quality target)
- **Three signal paths**: double-precision, Q31 fixed-point, or both
  (compile-time or runtime selectable)
- **SIMD**: AVX/SSE2 (x86) and NEON (AArch64), runtime dispatch
  (`-s auto|avx|sse2|neon|none`)
- **Quality tiers** (`-q speed|normal|best`): per-granule scale search that
  matches the source's level and bandwidth, trading encode time for SNR
- **Bit reservoir + rate control** (CBR) and **short blocks with proper
  start/stop transition windows**, one-granule lookahead, per-window
  `subblock_gain` and short-window scalefactors (MPEG-1 rates)
- **Psychoacoustic bit allocation**: Schroeder-Bark masking model driving a
  scalefactor noise-shaping loop (long and short granules)
- **All sample rates**: 8-48 kHz (MPEG-1, MPEG-2, MPEG-2.5)
- **All WAV formats**: PCM 8/16/24/32-bit, IEEE float 32/64, A-law,
  mu-law, WAVE_FORMAT_EXTENSIBLE, raw PCM (`-r`)
- **Streaming API**: callback-based output for real-time use
- **Bindings**: Python (ctypes), Rust (FFI + safe), Dart (Flutter FFI)
- **Embedded**: MP3 ~64 KB / AAC 47.6 KB RAM (`GLINT_MODE=fixed`),
  integer AAC hot path for no-FPU parts; fits ESP32/RP2040/STM32F4
- **AAC-LC encoder**: all four window sequences (short blocks with
  attack-split grouping), per-band M/S stereo, psychoacoustic noise
  shaping (`-q normal/best`), optimal-sectioning Huffman coding
  (per-band codebook DP), CBR-average rate control, ADTS output, all
  12 standard sample rates (8-96 kHz), mono/stereo. Validated against
  ffmpeg and CoreAudio decoders. TNS (selective, prediction-gain
  gated), tonality-aware masks at low rates, and a no-FPU integer
  hot path (`GLINT_MODE=fixed`) are all live. In the 6-clip league it
  places 3rd behind Apple and Fraunhofer FDK at 128 kbps (1st on two
  clips) and beats ffmpeg's native AAC, LAME-MP3 and vo-aacenc on
  every clip at every rate — see the AAC benchmarks below.

## Benchmarks

**Quality** — speech, 256 kbps stereo, vs. the original (1-min clip, measured
with `tests/measure_audio.py`; `double` and `fixed` paths are identical):

| Mode | SNR | seg-SNR | centroid | %E>10 kHz | 95% rolloff | mean NMR | speed |
|---|---|---|---|---|---|---|---|
| -q speed | 36.7 dB | 38.0 dB | 882 Hz | 0.71% | 5.34 kHz | −8.6 dB | ~260× |
| **-q normal** | **36.1 dB** | **35.6 dB** | 885 Hz | 0.71% | 5.37 kHz | **−11.0 dB** | ~52× |
| -q best | 36.2 dB | 35.7 dB | 891 Hz | 0.72% | 5.41 kHz | −11.1 dB | ~34× |

Source rolloff 5.4 kHz, centroid 892 Hz, %E>10 kHz 0.72%; normal/best
deliberately trade ~0.6 dB SNR for noise-to-mask (the psy loops shape
~9 dB below the mask). **In joint mode glint measures ahead of LAME on
this clip: 38.4 dB SNR vs LAME 256k's 36.9**, with mean noise-to-mask
−13.8 dB (LAME −16.1) and 0.2% of Bark band-frames above the estimated
mask (LAME 0.0%). Music (256 kbps joint): electronic 44.8 dB / **NMR
−18.0** (LAME 44.5 / −15.8 — glint ahead on both), string quartet 44.9 /
**−14.0** with 0.0% audible (LAME 46.0 / −11.1 — glint ahead on NMR).
Transients (castanet burst train): glint ahead of LAME on mean NMR at
both rates (128k: −2.1 vs 2.6; 256k: −9.7 vs −8.6) with audible
band-frames 2.4% vs 6.2%. Low rates: at 64 kbps stereo the
bitrate-scaled lowpass puts glint ahead of LAME on PEAQ ODG (−3.17 vs
−3.32). VBR with real variable-size frames, a Xing header and a gapless
LAME tag (players seek, report duration, and trim the codec delay
sample-exactly): V0 319 kbps / 40.7 dB / NMR −15.9, V4 266 kbps / −13.5
(psy shaping now runs in the VBR path too), V9 49 kbps. MPEG-2 rates
work and beat LAME (22.05 kHz CBR-64k: 21.6 dB / NMR 2.2 vs 17.6 /
2.4), including LSF short blocks. The fixed-point path now matches all
of this — short blocks and transition windows included — to within
0.06 dB. The machinery
behind this: exact pow34 companding, bit reservoir with buffer-feedback
rate control, short blocks with start/stop transition windows,
one-granule lookahead, subblock_gain and short-window scalefactors,
optimal Huffman region splits on finished granules, an encoder-side
sfb21 lowpass, and psychoacoustic bit allocation (Schroeder-Bark masks
driving scalefactor noise-shaping loops for long and short granules); see
PLAN.md. Both signal paths
are metrics-identical. Apple M1, 256 kbps stereo, single-threaded (`-j1`),
measured under moderate load — re-measure absolutes on an idle machine. The
optional threaded scale-factor search (`-j N`, byte-identical output) helps
most at `-q best`; `-j8` regresses past `-j4`. Quality metrics are unaffected by
thread count. For a deterministic local speed/quality run without external
audio, use `python tests/benchmark_encoder.py build/glint_cli`; to A/B two
builds with statistics, byte-identity, and quality regression flags, use
`python tests/ab_benchmark.py --a A/glint_cli --b B/glint_cli --quality`.

**Per-band SNR vs source** (256 kbps stereo, speech):

| Mode | 0–1 kHz | 1–4 kHz | 4–8 kHz | 8–16 kHz |
|---|---|---|---|---|
| -q best | 44.4 dB | 31.9 dB | 27.8 dB | 24.5 dB |

(speed/normal within 0.5 dB of best per band.) Noise sits where masking
absorbs it: most error power above 4 kHz, ~13% in 0–1 kHz. A Bark-band
noise-to-mask metric (`tests/measure_audio.py`) measures mean NMR −11.1 dB
stereo / −13.8 dB joint with 0.2–0.7% of band-frames above the mask
(LAME: −16.1 dB / 0.0%). For a full cross-encoder league table with
PEAQ ODG, ViSQOL, PESQ and STOI, run `python tests/compare_encoders.py`
(see PLAN.md §6b for tooling setup); `--check tests/quality_baselines.json`
is the regression gate.

**Footprint** (measured 2026-07: static/BSS + encoder context; Shine =
its `shine_global_config`):

| | glint double (desktop) | glint fixed¹ | Shine |
|---|---|---|---|
| Library (flash) | ~160 KB | ~90 KB | 225 KB |
| RAM | ~213 KB | **~64 KB** | ~96 KB |
| License | **MIT** | **MIT** | LGPL v2 |

¹ `GLINT_MODE=fixed` defines `GLINT_SMALL_BUFFERS`: frame buffers sized
to the largest legal frame (320 kbps @ 32 kHz), single-slot model
caches, and a table-free cbrt — measured metrics-identical to the
both-build's fixed path, full quality suite green. `-q speed` avoids
heap allocations entirely; the higher tiers use small transient vectors
in the scale search. Whisper ASR round-trip: 91% word similarity.

### AAC-LC benchmarks (2026-07-06)

6-clip league (speech, electronic, quartet, industrial, piano,
castanets; `python tests/compare_encoders.py --codec aac`), mean NMR in
dB (lower = better; ≤0 ≈ masked) and PEAQ ODG (0 = transparent).
Contenders: Apple (`afconvert`, CBR), Fraunhofer FDK (`fdkaac`),
ffmpeg's native AAC encoder, vo-aacenc (fixed-point, ex-Android), and
LAME-MP3 -q2 as a cross-format anchor. glint = `-q normal`.

**128 kbps stereo, mean NMR (rank of 6 encoders):**

| clip | Apple | FDK | **glint** | LAME-MP3 | ffmpeg | vo-aacenc |
|---|---|---|---|---|---|---|
| speech | **−6.9** | −5.4 | −3.6 ⑶ | −2.2 | −0.7 | +1.4 |
| electronic | −9.9 | **−11.4** | −3.6 ⑶ | −3.1 | −1.9 | +2.3 |
| quartet | −2.9 | −2.6 | **−5.5 ⑴** | −2.0 | +0.2 | +1.2 |
| industrial | −1.1 | −0.8 | **−1.6 ⑴** | −0.0 | +1.6 | +1.3 |
| piano | **−9.4** | −8.5 | −8.7 ⑵ | −7.5 | −4.1 | −1.8 |
| castanets | −7.4 | **−9.0** | −3.5 ⑶ | +2.6 | +7.1 | +18.6 |

glint places 1st–3rd on every clip: behind only Apple and FDK overall,
ahead of ffmpeg-native, LAME and vo-aacenc everywhere. On castanets
glint's PEAQ ODG (−0.04) actually ties Apple/FDK (−0.08) — the short
blocks land the transients. At **256 kbps** ODG is ≈0 (transparent) for
Apple, FDK, glint and LAME on all clips; glint has the highest SNR of
any AAC encoder on 4/6 clips (e.g. quartet 50.6 dB and **NMR −15.0,
1st**; speech 42.2 dB, NMR −16.6 vs Apple −18.1) while vo-aacenc never
reaches a negative castanets NMR at any rate.

*Post-league improvements* (the table above is the dated 2026-07-06
snapshot; TNS and tonality-aware masks landed after that run):
castanets 128k mean NMR is now **−8.4** (integer build −8.6) — ahead
of Apple's −7.4, closing on FDK's −9.0 — and electronic 128k PEAQ ODG
improved −0.78 → **−0.49**; speech 64k mono ODG −2.35 → −2.23. All
other cells hold or improve; re-run
`python tests/compare_encoders.py --codec aac` for current numbers.

**Speed** (M1, 60 s speech, 44.1 kHz stereo 128k): glint-aac speed
~85×, normal ~41×, best ~37× realtime; Apple ~104×, FDK ~90×,
vo-aacenc ~100×, ffmpeg-native ~34×. No perf pass has run on the AAC
path yet (the MP3 path got −27..−45% from two passes; the same LUT
machinery applies).

**RAM** (measured; encoder context + static tables): with
`GLINT_SMALL_BUFFERS` (the `GLINT_MODE=fixed` build) glint-aac needs
**47.6 KB** (25.0 KB context + 22.6 KB tables) — under vo-aacenc's
measured **48.0 KB** (heap via its own allocator hook, zero BSS) —
and all per-coefficient arithmetic is INTEGER (`GLINT_AAC_INT`:
int32 MDCT/quantizer/energies — no-FPU parts like RP2040 pay
soft-float only for per-frame scalars). Quality: mono identical to
the float builds; stereo pays ~1 dB NMR (the integer M/S half-LSB,
documented in PLAN.md § A2b). Desktop double ≈ 106 KB.

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

| Build mode | Flag | RAM footprint (measured 2026-07) |
|---|---|---|
| `double` (default) | — | ~213 KB |
| `fixed` | `-DGLINT_MODE=fixed` | **~64 KB** (no FPU; small buffers sized for every legal frame incl. 320 kbps) |
| `both` | `-DGLINT_MODE=both` | ~225 KB (runtime `-p` switch) |

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
glint_cli [options] input output.{mp3,aac}
  -F FORMAT         mp3|aac (default: by output extension)
  -b BITRATE        CBR bitrate in kbps (default: 128)
  -V QUALITY        VBR quality 0-9 (0=best, 9=smallest)
  -m MODE           mono|stereo|joint (default: auto)
  -q QUALITY        speed|normal|best (default: normal)
  -s SIMD           auto|avx|sse2|neon|none (default: auto)
  -p PATH           double|fixed (only in both-mode builds)
  -r RATE:CH:BITS   Raw PCM input (e.g., 44100:1:16)
  -j N              Worker threads for the scale-factor search (default: 1)
```

`-j` parallelizes the per-granule scale-factor search across threads. The
output bitstream is byte-identical regardless of thread count (candidates are
reduced in a fixed order), so it is a pure throughput knob with no quality
effect. Gains are largest for `-q best`.

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
| `glint_vbr_header(enc, buf, cap)` | Finalized Xing+LAME frame (VBR; call after flush, rewrite frame 0) |
| `glint_flush(enc, &size)` | Drain buffered frames — required at end of stream |
| `glint_destroy(enc)` | Free encoder |
| `glint_set_threads(n)` | Worker threads for the scale-factor search (process-global; output byte-identical for any `n`) |

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
- **MDCT**: 36-point long/start/stop windows and 12-point short blocks
  (3 windows), /288 normalization, transposed cosine table for SIMD; window
  scheduling with one granule of encoder lookahead (+576 samples latency)
- **Quantization**: exact x^0.75 companding, anti-clipping gain bounds,
  binary-search gain to the bit budget (`quantize_base`), wrapped in a
  per-granule input-scale search that minimizes decoder-reconstruction MSE
  (`quantize_granule`); CBR rate control via a constant-quality gain anchor;
  NMR-driven scalefactor noise shaping against a Schroeder-Bark mask model
  (per-band for long granules, per-(band,window) for short, with
  scalefac_scale escalation and energy-based subblock_gain)
- **Huffman**: table choice by actual bit count (fused select+count),
  34 ISO tables, SCFSI
- **Bitstream**: 32-bit accumulator; bit reservoir as a continuous main-data
  stream with deferred frame emission (`reservoir.hpp`); VBR frames
  self-contained with per-frame bitrate selection

## Project structure

```
glint/
├── include/glint/glint.h      C API (MP3 + AAC)
├── src/                       MP3 encoder core (7 modules + tables)
│   └── aac_*.{hpp,cpp}        AAC-LC encoder (MDCT, coder, framing, tables)
├── tools/gen_aac_tables.py    ISO AAC table extractor/cross-checker
├── cli/main.cpp               WAV-to-MP3/AAC CLI
├── tests/                     Unit tests + quality tests + ASR
├── bindings/
│   ├── python/                ctypes wrapper + pip packaging
│   ├── rust/                  glint-sys (FFI) + glint (safe)
│   └── dart/                  Flutter FFI
├── esp-idf/                   ESP32 component (~64 KB RAM)
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
| encode speed         | —                     | ~230× / 115× / 54× realtime (Apple M1, `-j1`) |

(Historical table. The dead-zone loss the scale search fought turned out to
be a symptom of a broken pow34 quantization curve; fixing that — see PLAN.md
item 0 — lifted all tiers to ~34.5 dB SNR and re-centered the scale search
around f = 1.0. The Benchmarks section above has the current numbers.)

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
