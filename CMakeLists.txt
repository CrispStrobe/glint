cmake_minimum_required(VERSION 3.16)
project(glint VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable native architecture optimizations in Release builds
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        add_compile_options(/O2 /fp:fast)
    else()
        add_compile_options(-march=native -ffast-math)
    endif()
endif()

# Signal path mode:
#   double (default) — double-precision only
#   fixed            — Q31 fixed-point only
#   both             — both compiled in, selectable at runtime via CLI -p flag
set(GLINT_MODE "double" CACHE STRING "Signal path: double, fixed, or both")
set_property(CACHE GLINT_MODE PROPERTY STRINGS double fixed both)

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

if(GLINT_MODE STREQUAL "fixed")
    target_compile_definitions(glint_static PUBLIC GLINT_FIXED_POINT)
    target_compile_definitions(glint_shared PUBLIC GLINT_FIXED_POINT)
elseif(GLINT_MODE STREQUAL "both")
    target_compile_definitions(glint_static PUBLIC GLINT_FIXED_POINT GLINT_BOTH_PATHS)
    target_compile_definitions(glint_shared PUBLIC GLINT_FIXED_POINT GLINT_BOTH_PATHS)
endif()

# CLI
add_executable(glint_cli cli/main.cpp)
target_link_libraries(glint_cli PRIVATE glint_static)
target_include_directories(glint_cli PRIVATE src)

# Unit tests
enable_testing()
add_executable(glint_test tests/test_unit.cpp)
target_link_libraries(glint_test PRIVATE glint_static)
target_include_directories(glint_test PRIVATE src)
add_test(NAME unit_tests COMMAND glint_test)
