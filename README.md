# glint

A clean-room **MP3 + AAC-LC encoder** — plus an **Opus decoder in
progress** — in C++17, MIT licensed. The name nods to integers
(*g-lint*) and the [Shine](https://github.com/toots/shine) encoder
lineage.

Both codecs are implemented from the ISO specs (11172-3 / 13818-3 for
MP3, 13818-7 / 14496-3 for AAC) with no third-party encoder code
referenced; the normative AAC tables are ISO data, extracted from two
independent implementations and cross-checked bit-for-bit
(`tools/gen_aac_tables.py`). To our knowledge glint is the only
competitive-quality AAC-LC encoder under a plain OSI license — ffmpeg's
native encoder is LGPL, FDK's license is not OSI-approved, and
vo-aacenc (Apache-2.0) is unmaintained and last on quality.

**Highlights** (all measured; experiment log in `PLAN.md`):

- **AAC quality, 128 kbps**: behind only Apple and Fraunhofer FDK in a
  6-clip league — 1st on noise-to-mask for string quartet, industrial
  and piano, best-in-league PEAQ ODG on the castanets transient
  torture clip (0.00, transparent).
- **MP3 quality, 128 kbps**: ahead of LAME 3.100 on PEAQ ODG on 8 of
  10 corpus clips.
- **Speed**: fastest encoder in the AAC league at every quality tier
  (~272×/168× realtime at `-q speed`/`-q best` on an M1); MP3
  ~260×/34× (speed/best).
- **Embedded**: MP3 in ~64 KB, AAC in **47.4 KB** of RAM
  (`GLINT_MODE=fixed`), and at `-q speed` both codecs encode with
  **no per-coefficient floating point** — proven by disassembly on
  Cortex-M0+ (`tools/check_nofpu.sh`) and functionally validated under
  QEMU. Ready-to-flash benchmarks for Raspberry Pi Pico and ESP32.
- **Verified wire format**: every configuration decodes with zero
  errors in both ffmpeg and Apple CoreAudio, which produce metrically
  identical output; decode-based gates run in CI.
- **Opus track (new, this branch)**: a clean-room Opus **decoder**
  built from RFC 6716. CELT-only streams (the music/low-latency modes)
  already decode end-to-end identically to libopus: on 2200+ real
  packets the decoder's final range equals the encoder's — the Opus
  conformance identity — and PCM matches libopus within 3 int16 LSB.
  Every layer (range coder, Laplace, PVQ enumeration, allocator, band
  decoder, IMDCT) is cross-checked against libopus by a dedicated
  harness in `tools/`. SILK (speech modes) is next; see PLAN.md
  O-track.

## Quick start

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
build/glint_cli -b 128 in.wav out.mp3
build/glint_cli -b 128 in.wav out.aac      # AAC by extension (or -F aac)
build/glint_cli -V 2 in.wav out.aac        # constant-quality VBR
```

Pre-built libraries for 9 platforms:
[releases](https://github.com/CrispStrobe/glint/releases).

## Benchmarks

### AAC (v0.8.0 league, 2026-07)

6 clips × 2 rates vs Apple (`afconvert`), Fraunhofer FDK, ffmpeg's
native AAC, vo-aacenc, and LAME-MP3 as a cross-format anchor
(`python tests/compare_encoders.py --codec aac`; full log with
SNR/NMR/ODG/PESQ/STOI in `tests/aac_league_v0.8.txt`).

**128 kbps stereo, mean noise-to-mask in dB** (lower is better,
rank ①–⑥; glint `-q normal`):

| clip | Apple | FDK | **glint** | LAME-MP3 | ffmpeg | vo-aacenc |
|---|---|---|---|---|---|---|
| speech | **−6.9** | −5.4 | −3.6 ⑶ | −2.2 | −0.7 | +1.4 |
| electronic | −9.9 | **−11.4** | −4.3 ⑶ | −3.1 | −1.9 | +2.3 |
| quartet | −2.9 | −2.6 | **−5.8 ①** | −2.0 | +0.2 | +1.2 |
| industrial | −1.1 | −0.8 | **−1.4 ①** | −0.0 | +1.6 | +1.3 |
| piano | −9.4 | −8.5 | **−10.2 ①** | −7.5 | −4.1 | −1.8 |
| castanets | −7.4 | **−9.0** | −8.5 ⑵ | +2.6 | +7.1 | +18.6 |

On castanets glint's PEAQ ODG (0.00) is the best of all six encoders —
short blocks, selective TNS and per-group shaping land the transients
transparently. At 256 kbps Apple/FDK/glint are all ODG-transparent;
glint has the best NMR on quartet (−18.7) and the highest SNR of the
AAC field on most clips. AAC VBR: V0 ≈ 311 kbps with 0.0 % audible
band-frames on speech (better than CBR-256), V4 ≈ 130–160 kbps
content-adaptive, V9 ≈ 42 kbps.

### MP3 (vs LAME 3.100 `-q 2`)

128 kbps joint PEAQ ODG over the 10-clip corpus: glint ahead on 8
(e.g. choir −1.28 vs −2.06, quartet −0.46 vs −0.88), LAME ahead on
drums and castanets-ODG. 256 kbps: everything ODG-transparent, glint
ahead on SNR throughout (speech 38.4 vs 36.9 dB). MPEG-2 22.05 kHz @
64k: 21.6 dB vs 17.6. VBR with Xing + gapless LAME tag (sample-exact
delay trim). Run `python tests/compare_encoders.py` for current
numbers; `--check tests/quality_baselines.json` is the regression gate.

### Footprint (measured: static tables + encoder context)

| | MP3 double | MP3 fixed | Shine | AAC double | AAC fixed | vo-aacenc |
|---|---|---|---|---|---|---|
| RAM | ~213 KB | **~64 KB** | ~96 KB | ~106 KB | **47.4 KB** | 48 KB |
| License | MIT | MIT | LGPL | MIT | MIT | Apache-2.0 |

The `fixed` builds also make the `-q speed` hot paths fully integer
(see Embedded below). The AAC fixed build trades ~1 dB NMR on stereo
vs the float builds (the irreducible half-LSB of integer M/S; mono is
identical). Transient stack use: ~30 KB — size RTOS stacks accordingly.

## Usage

### CLI

```
glint_cli [options] input output.{mp3,aac}
  -F FORMAT         mp3|aac (default: by output extension)
  -b BITRATE        CBR bitrate in kbps (default: 128)
  -V QUALITY        VBR quality 0-9 (0=best, 9=smallest)
  -m MODE           mono|stereo|joint (default: auto)
  -q QUALITY        speed|normal|best (default: normal)
  -s SIMD           auto|avx|sse2|neon|none (MP3; default: auto)
  -p PATH           double|fixed (MP3, both-mode builds only)
  -r RATE:CH:BITS   Raw PCM input (e.g., 44100:1:16)
  -j N              Worker threads, MP3 scale search (byte-identical output)
```

### C API

```c
#include <glint/glint.h>

/* MP3 */
struct glint_config mc = { .sample_rate = 44100, .num_channels = 2,
                           .mode = GLINT_JOINT, .bitrate = 128,
                           .quality = GLINT_QUALITY_NORMAL };
glint_t mp3 = glint_create(&mc);

/* AAC — zero-initialize the struct (reserved tail selects defaults) */
struct glint_aac_config ac = {0};
ac.sample_rate = 44100; ac.num_channels = 2;
ac.bitrate = 128; ac.quality = GLINT_QUALITY_NORMAL;
glint_aac_t aac = glint_aac_create(&ac);

const int16_t* ch[2] = { left, right };   /* samples_per_frame() each */
int n;
const uint8_t* frame = glint_aac_encode(aac, ch, &n);
/* ... write frame ... at end of stream: */
frame = glint_aac_flush(aac, &n);          /* REQUIRED (tail frames) */
```

MP3: `glint_create[_streaming] / glint_encode[_float|_int32] /
glint_flush / glint_vbr_header / glint_destroy / glint_set_threads`.
AAC: `glint_aac_create / glint_aac_encode[_float] / glint_aac_flush /
glint_aac_destroy` (encoder delay 2048 samples; flush returns two tail
frames). `glint_version()` reports the library version. AAC VBR: set
`cfg.vbr = 1; cfg.vbr_quality = 0..9`.

### Bindings

```python
import glint
glint.encode_pcm(...)                      # MP3
enc = glint.AacEncoder(44100, 2, 128)      # AAC (vbr_quality= for VBR)
```

```rust
let mp3 = glint::encode_pcm(&pcm, 44100, 2, 128);
let aac = glint::encode_pcm_aac(&pcm, 44100, 2, 128, 1);
```

Dart/Flutter: `GlintEncoder` / `GlintAacEncoder` (FFI).

## Embedded and no-FPU

`GLINT_MODE=fixed` targets microcontrollers: small buffers, and at
`-q speed` **integer per-coefficient hot paths for both codecs**
(`GLINT_MP3_INT` / `GLINT_AAC_INT`: integer MDCTs, a shared Q16
log-domain quantizer, int64 energies). Only per-frame scalars and the
`-q normal/best` psy tiers use floating point (soft-float on FPU-less
parts is fine at per-frame rates).

Validation without hardware (`embedded/README.md`):

- `tools/check_nofpu.sh` — disassembles a Cortex-M0+ build and fails
  if any per-coefficient function contains a soft-float call.
- `embedded/qemu/build_and_run.sh` — runs the benchmark on an emulated
  Cortex-M3; the semihosted output decodes cleanly in ffmpeg/CoreAudio
  and the MP3 stream is bit-exact with a host run.

On-target throughput benchmarks (serial output with checksums and
×-realtime): `embedded/pico/` (RP2040) and `esp-idf/example/` (ESP32).

## Architecture

```
MP3: PCM → polyphase subband (SIMD) → 36-pt MDCT + alias reduction
     → x^0.75 quantizer + scale search + NMR shaping → Huffman (34 tables)
     → bit reservoir → frames
AAC: PCM → 2048/8×256-pt MDCT (4 window sequences) → TNS → per-band M/S
     → distortion-controlled allocation (noise targets ∝ mask^α, budget
       bisection) or NMR shaping walk → 11-codebook Huffman with optimal
       sectioning (exact bit accounting) → ADTS frames

Opus (decode): packet → TOC/framing → range decoder → energy envelope
     (Laplace + 2-D prediction) → implicit bit allocation (re-derived
       by the decoder, no side info) → PVQ shapes + folding +
       anti-collapse → inverse MDCT (mixed-radix FFT) → pitch
       postfilter → de-emphasis → PCM
```

Shared discipline: psychoacoustic masks aligned with the measurement
metric (Schroeder-Bark), exact bit counting (count==emission is
unit-tested), transient scheduling with one-block lookahead, and every
quality change gated on SNR/NMR/PEAQ/PESQ across a clip corpus — with
decode-based tests against real decoders, because unit tests alone
historically passed while real decoders produced garbage.

## Project structure

```
glint/
├── include/glint/glint.h      C API (MP3 + AAC)
├── src/                       MP3 core + aac_*.{hpp,cpp} (AAC core)
│                              + opus_*.{hpp,cpp} (Opus decoder, WIP)
├── tools/                     AAC/CELT table generators, no-FPU checker,
│                              Opus-vs-libopus cross-check harnesses
├── cli/main.cpp               WAV → MP3/AAC CLI
├── tests/                     unit + decode gates + quality/league/ABX
├── bindings/                  Python (ctypes) · Rust (sys+safe) · Dart FFI
├── embedded/                  bench core, QEMU target, Pico project
├── esp-idf/                   ESP32 component + example
├── packaging/vcpkg/           vcpkg port
└── .github/workflows/         CI + release (9 platforms)
```

## Building

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

| Build mode | Flag | Notes |
|---|---|---|
| `double` (default) | — | full quality, desktop |
| `fixed` | `-DGLINT_MODE=fixed` | small buffers + integer hot paths |
| `both` | `-DGLINT_MODE=both` | runtime `-p` switch (MP3) |

Cross-compilation (Android NDK, iOS, aarch64 Linux) as usual via CMake
toolchain files with `-DGLINT_MODE=fixed`; see `.github/workflows/` for
the exact invocations used in CI.

## Roadmap

- **Opus** (PLAN.md O-track): SILK decoder + hybrid mode → full RFC
  test-vector conformance → Ogg Opus container → CELT-only encoder.
- On-target RP2040/ESP32 throughput measurements (harness ships, needs
  silicon).
- The last ~0.5 PEAQ ODG to Apple on speech/electronic at 128 kbps
  (mask accuracy: finer tonality, temporal effects).
- AC-3 as a possible third codec (patents expired 2017; no MIT encoder
  exists).
- Q31 arithmetic for the AAC `-q normal/best` tiers.

Retired roadmap items and every measured dead end live in `PLAN.md`.

## License

MIT. The AAC and MP3 formats' remaining patent situations are the
user's responsibility to evaluate for their jurisdiction; see PLAN.md
and the release notes for the project's understanding.
