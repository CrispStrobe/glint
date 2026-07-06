# glint ESP-IDF component
#
# Usage: copy or symlink this directory into your ESP-IDF project's components/
#   cd your_project/components
#   ln -s /path/to/glint/esp-idf glint
#
# Then in your main CMakeLists.txt:
#   idf_component_register(... REQUIRES glint)
#
# MP3: fixed-point mode with small buffers (~64 KB RAM).
# AAC-LC: small-buffers storage (~48 KB RAM); float ARITHMETIC (see PLAN A2) —
# fine on ESP32 (FPU) / ESP32-S3; slow via soft-float on FPU-less parts.

get_filename_component(GLINT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)

idf_component_register(
    SRCS
        "${GLINT_ROOT}/src/subband.cpp"
        "${GLINT_ROOT}/src/mdct.cpp"
        "${GLINT_ROOT}/src/quantize.cpp"
        "${GLINT_ROOT}/src/huffman.cpp"
        "${GLINT_ROOT}/src/reservoir.cpp"
        "${GLINT_ROOT}/src/bitstream.cpp"
        "${GLINT_ROOT}/src/psycho.cpp"
        "${GLINT_ROOT}/src/encoder.cpp"
        "${GLINT_ROOT}/src/aac_mdct.cpp"
        "${GLINT_ROOT}/src/aac_coder.cpp"
        "${GLINT_ROOT}/src/aac_psy.cpp"
        "${GLINT_ROOT}/src/aac_tns.cpp"
        "${GLINT_ROOT}/src/aac_encoder.cpp"
    INCLUDE_DIRS
        "${GLINT_ROOT}/include"
    PRIV_INCLUDE_DIRS
        "${GLINT_ROOT}/src"
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    GLINT_FIXED_POINT
    GLINT_SMALL_BUFFERS
    GLINT_SMALL_POW34
)

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -O2)
