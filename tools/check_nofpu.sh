#!/bin/sh
# Static no-FPU proof for the -q speed hot paths.
#
# Cross-compiles the codec sources for Cortex-M0+ (RP2040: no FPU, no
# hardware double anything) and disassembles them: any float/double
# operation shows up as an __aeabi_* libcall. The HOT (per-coefficient)
# functions must contain none; per-frame scalar functions are allowed to.
#
# Usage: tools/check_nofpu.sh   (from the repo root)
set -e
TC="${GLINT_ARM_GCC:-$HOME/code/glint-tools/xpack-arm-none-eabi-gcc-14.2.1-1.1/bin}"
CXX="$TC/arm-none-eabi-g++"
OBJDUMP="$TC/arm-none-eabi-objdump"
FLAGS="-mcpu=cortex-m0plus -mthumb -mfloat-abi=soft -O2
       -fno-exceptions -fno-rtti -fno-threadsafe-statics
       -DGLINT_FIXED_POINT -DGLINT_SMALL_BUFFERS -DGLINT_SMALL_POW34
       -DGLINT_AAC_INT -DGLINT_MP3_INT -DGLINT_NO_THREADS
       -Iinclude -Isrc"

# function-name substrings that must be float-free (per-coefficient work)
HOT="quantize_and_count_int|quantize_granule_int_speed|process_int|\
alias_reduce_fp|band_costs|code_band|aac_quantize|aac_reorder_short|\
aac_mdct_frame|mdct_core_int|fft|huffman_select_and_count|\
huffman_count_bits|write_ics_body|exp2_quant|ilog2"

fail=0
for f in quantize mdct huffman bitstream aac_mdct aac_coder aac_encoder; do
  $CXX $FLAGS -c src/$f.cpp -o /tmp/nofpu_$f.o
  $OBJDUMP -dC /tmp/nofpu_$f.o | awk -v hot="$HOT" '
    /^[0-9a-f]+ <.*>:$/ {
      fn = $2; gsub(/[<>:]/, "", fn);
      # global ctors (startup-time table builders) are allowed to use
      # soft-float; they inherit a hot symbol name in _GLOBAL__sub_I_*
      inhot = (fn ~ hot) && fn !~ /_GLOBAL__sub_I|static_initialization/;
    }
    inhot && /__aeabi_(d|f|i2d|i2f|ui2d|ui2f|l2d|l2f)/ {
      print "  FLOAT in " fn ": " $NF; found = 1;
    }
    END { exit found ? 1 : 0 }
  ' || fail=1
  echo "$f.cpp: checked"
done

if [ $fail -eq 0 ]; then
  echo "PASS: no soft-float calls in any hot (per-coefficient) function"
else
  echo "FAIL: soft-float found in hot paths"
  exit 1
fi
