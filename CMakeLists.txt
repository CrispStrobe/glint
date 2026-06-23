cmake_minimum_required(VERSION 3.16)
project(glint VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GLINT_SOURCES
    src/subband.cpp
    src/mdct.cpp
    src/quantize.cpp
    src/huffman.cpp
    src/reservoir.cpp
    src/bitstream.cpp
    src/encoder.cpp
)

# Static library
add_library(glint_static STATIC ${GLINT_SOURCES})
target_include_directories(glint_static PUBLIC include PRIVATE src)
set_target_properties(glint_static PROPERTIES OUTPUT_NAME glint)

# Shared library
add_library(glint_shared SHARED ${GLINT_SOURCES})
target_include_directories(glint_shared PUBLIC include PRIVATE src)
set_target_properties(glint_shared PROPERTIES OUTPUT_NAME glint)

# CLI
add_executable(glint_cli cli/main.cpp)
target_link_libraries(glint_cli PRIVATE glint_static)
target_include_directories(glint_cli PRIVATE src)
