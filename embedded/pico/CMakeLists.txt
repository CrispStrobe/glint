# glint benchmark for Raspberry Pi Pico (RP2040, Cortex-M0+, no FPU).
#
# Build (needs the pico-sdk; https://github.com/raspberrypi/pico-sdk):
#   export PICO_SDK_PATH=/path/to/pico-sdk
#   cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
# Flash build/glint_bench.uf2 (BOOTSEL drag-and-drop), then open the USB
# serial port (115200) to read the results. The benchmark prints frames,
# bytes, an FNV checksum (compare against a host run of the same core;
# the MP3 -q speed stream is expected BIT-EXACT across platforms) and
# x-realtime percent from the RP2040's microsecond timer.
cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(glint_bench C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

set(GLINT_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

add_executable(glint_bench
    main.c
    ${GLINT_ROOT}/embedded/bench/bench_core.c
    ${GLINT_ROOT}/src/subband.cpp
    ${GLINT_ROOT}/src/mdct.cpp
    ${GLINT_ROOT}/src/quantize.cpp
    ${GLINT_ROOT}/src/huffman.cpp
    ${GLINT_ROOT}/src/reservoir.cpp
    ${GLINT_ROOT}/src/bitstream.cpp
    ${GLINT_ROOT}/src/psycho.cpp
    ${GLINT_ROOT}/src/encoder.cpp
    ${GLINT_ROOT}/src/aac_mdct.cpp
    ${GLINT_ROOT}/src/aac_coder.cpp
    ${GLINT_ROOT}/src/aac_psy.cpp
    ${GLINT_ROOT}/src/aac_tns.cpp
    ${GLINT_ROOT}/src/aac_encoder.cpp
)

target_include_directories(glint_bench PRIVATE
    ${GLINT_ROOT}/include ${GLINT_ROOT}/src)

target_compile_definitions(glint_bench PRIVATE
    GLINT_FIXED_POINT GLINT_SMALL_BUFFERS GLINT_SMALL_POW34
    GLINT_AAC_INT GLINT_MP3_INT GLINT_NO_THREADS)

target_compile_options(glint_bench PRIVATE
    -O2 -fno-exceptions -fno-rtti -fno-threadsafe-statics)

target_link_libraries(glint_bench pico_stdlib)

pico_enable_stdio_usb(glint_bench 1)
pico_enable_stdio_uart(glint_bench 0)
pico_add_extra_outputs(glint_bench)
