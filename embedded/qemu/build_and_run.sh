#!/bin/sh
# Build the glint benchmark for QEMU's mps2-an385 (Cortex-M3, soft-float)
# and run it under semihosting. FUNCTIONAL validation only: QEMU is not
# cycle-accurate, so no timing is reported; the benchmark dumps
# bench_out.mp3 / bench_out.aac into the CURRENT DIRECTORY via semihosted
# file IO for host-side decode checks (ffmpeg -v error -i bench_out.mp3 ...).
#
# Cortex-M3 shares the property that matters with the RP2040's M0+:
# no FPU (all residual float is soft-float). For the per-instruction
# no-FPU proof of the hot paths, see tools/check_nofpu.sh.
set -e
cd "$(dirname "$0")"
ROOT=../..
# Toolchain: needs newlib — the Homebrew arm-none-eabi-gcc formula is
# compiler-only. Default to the xPack distribution; override with
# GLINT_ARM_GCC=/path/to/bin.
TC="${GLINT_ARM_GCC:-$HOME/code/glint-tools/xpack-arm-none-eabi-gcc-14.2.1-1.1/bin}"
CXX="$TC/arm-none-eabi-g++"
CC="$TC/arm-none-eabi-gcc"
FLAGS="-mcpu=cortex-m3 -mthumb -mfloat-abi=soft -O2 -ffunction-sections -fdata-sections
       -fno-exceptions -fno-rtti -fno-threadsafe-statics
       -DGLINT_FIXED_POINT -DGLINT_SMALL_BUFFERS -DGLINT_SMALL_POW34
       -DGLINT_AAC_INT -DGLINT_MP3_INT -DGLINT_NO_THREADS
       -I$ROOT/include -I$ROOT/src"
LDFLAGS="--specs=rdimon.specs -Wl,--gc-sections -T mps2.ld -Wl,--no-warn-rwx-segments"

$CC  $FLAGS -c startup.c -o /tmp/gq_startup.o
$CC  $FLAGS -c glue.c -o /tmp/gq_glue.o
$CC  $FLAGS -c $ROOT/embedded/bench/bench_core.c -o /tmp/gq_bench.o
OBJS="/tmp/gq_startup.o /tmp/gq_glue.o /tmp/gq_bench.o"
for f in subband mdct quantize huffman reservoir bitstream psycho encoder \
         aac_mdct aac_coder aac_psy aac_tns aac_encoder; do
  $CXX $FLAGS -c $ROOT/src/$f.cpp -o /tmp/gq_$f.o
  OBJS="$OBJS /tmp/gq_$f.o"
done
$CXX $FLAGS $LDFLAGS $OBJS -o /tmp/glint_bench.elf
"$TC/arm-none-eabi-size" /tmp/glint_bench.elf

qemu-system-arm -M mps2-an385 -cpu cortex-m3 -nographic -semihosting \
    -semihosting-config enable=on,target=native \
    -kernel /tmp/glint_bench.elf
