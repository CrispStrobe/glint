# glint embedded benchmark & no-FPU validation

Both codecs' `-q speed` paths are integer per-coefficient (see PLAN.md
§13 and §A2b). This directory holds the shared benchmark and per-target
projects, plus two host-side validation layers that need NO hardware:

## What can be verified without a device

1. **Static no-FPU proof** — `tools/check_nofpu.sh` cross-compiles the
   codec sources for Cortex-M0+ (`-mfloat-abi=soft`), disassembles them,
   and FAILS if any per-coefficient function contains an `__aeabi_*`
   soft-float call (startup-time table constructors are exempt — they
   run once from `.init_array`). Passing means the hot loops literally
   contain no float instructions on an RP2040-class core.

2. **QEMU functional run** — `qemu/build_and_run.sh` builds a semihosted
   Cortex-M3 image (mps2-an385: same no-FPU property as the M0+) and runs
   it under `qemu-system-arm`. It encodes 4 s of synthetic stereo through
   both codecs and writes `bench_out.mp3` / `bench_out.aac` back to the
   host via semihosting for `ffmpeg`/`afconvert` decode validation.
   Measured result (2026-07): both streams decode with zero errors, and
   the MP3 stream is BIT-EXACT with a host (Apple Silicon) run of the
   same benchmark — the integer path is deterministic across
   architectures. (The AAC stream differs by a few bytes: its per-band
   masks use double math that meets `-ffast-math` on the host.)
   **QEMU is not cycle-accurate: it validates correctness, not speed.**

## What needs real silicon

Throughput. Flash one of these and read the serial output — the
benchmark prints frames, bytes, an FNV checksum (compare with a host run)
and percent-realtime from the target's microsecond timer:

- `pico/` — Raspberry Pi Pico (RP2040, Cortex-M0+): pico-sdk project,
  see the header of `pico/CMakeLists.txt`. Output on USB serial.
- `../esp-idf/example/` — ESP32 / ESP32-S3: `idf.py build flash monitor`
  with the `esp-idf/` component symlinked in (header of its
  CMakeLists.txt has the exact commands).

Both configs use `-q speed`, 44.1 kHz stereo, 128 kbps, matching the
`bench/bench_core.c` host reference. RAM: MP3 ~64 KB, AAC 47.6 KB
context+tables (plus ~30 KB transient stack in the MDCT/rate loop —
size FreeRTOS/pico stacks accordingly), well within the RP2040's 264 KB
and any ESP32.

## Toolchain notes (macOS)

The Homebrew `arm-none-eabi-gcc` formula has no C library; the QEMU
build and the static check default to the xPack toolchain at
`~/code/glint-tools/xpack-arm-none-eabi-gcc-14.2.1-1.1/` (override with
`GLINT_ARM_GCC=/path/to/bin`). `brew install qemu` provides
`qemu-system-arm`.
