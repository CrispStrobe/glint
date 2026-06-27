cmake_minimum_required(VERSION 3.16)
project(glint VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable optimizations in Release builds
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    if(MSVC)
        add_compile_options(/O2 /fp:fast)
    elseif(NOT CMAKE_CROSSCOMPILING AND CMAKE_SYSTEM_PROCESSOR STREQUAL CMAKE_HOST_SYSTEM_PROCESSOR)
        # -march=native only works for native builds, not cross-compilation
        add_compile_options(-O3 -march=native -ffast-math)
    else()
        add_compile_options(-O3 -ffast-math)
    endif()
    # Link-time optimization for cross-TU inlining
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

# Signal path mode:
#   double (default) — double-precision only
#   fixed            — Q31 fixed-point only
#   both             — both compiled in, selectable at runtime via CLI -p flag
# PGO (profile-guided optimization):
#   off      — no PGO (default)
#   generate — instrument build, produces .gcda profile data when run
#   use      — optimize using profile data from a prior "generate" run
set(GLINT_PGO "off" CACHE STRING "PGO mode: off, generate, or use")
set_property(CACHE GLINT_PGO PROPERTY STRINGS off generate use)

if(GLINT_PGO STREQUAL "generate")
    set(GLINT_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-data" CACHE PATH "Directory for PGO profile data")
    file(MAKE_DIRECTORY "${GLINT_PGO_DIR}")
    add_compile_options(-fprofile-generate=${GLINT_PGO_DIR})
    add_link_options(-fprofile-generate=${GLINT_PGO_DIR})
elseif(GLINT_PGO STREQUAL "use")
    set(GLINT_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-data" CACHE PATH "Directory for PGO profile data")
    add_compile_options(-fprofile-use=${GLINT_PGO_DIR} -fprofile-correction)
    add_link_options(-fprofile-use=${GLINT_PGO_DIR})
    # GCC 13 miscompiles under PGO + LTO (crash in cleanup); disable LTO for PGO builds
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION FALSE)
endif()

# Experimental: parallel stereo channel encoding (off by default).
# Regressed on x86 Xeon (-5 to -19%) due to std::async overhead, but may
# help on platforms with fast thread dispatch (Apple M1/M2) or with a
# thread pool. Enable to test: cmake -DGLINT_PARALLEL_CH=ON ..
option(GLINT_PARALLEL_CH "Encode stereo channels in parallel via std::async" OFF)

set(GLINT_MODE "double" CACHE STRING "Signal path: double, fixed, or both")
set_property(CACHE GLINT_MODE PROPERTY STRINGS double fixed both)

set(GLINT_SOURCES
    src/subband.cpp
    src/mdct.cpp
    src/quantize.cpp
    src/huffman.cpp
    src/reservoir.cpp
    src/bitstream.cpp
    src/psycho.cpp
    src/encoder.cpp
)

# Threading support (only needed for parallel channel encoding)
if(GLINT_PARALLEL_CH)
    find_package(Threads REQUIRED)
endif()

# Static library
add_library(glint_static STATIC ${GLINT_SOURCES})
target_include_directories(glint_static PUBLIC include PRIVATE src)
set_target_properties(glint_static PROPERTIES OUTPUT_NAME glint)

# Shared library
add_library(glint_shared SHARED ${GLINT_SOURCES})
target_include_directories(glint_shared PUBLIC include PRIVATE src)
set_target_properties(glint_shared PROPERTIES OUTPUT_NAME glint)

if(GLINT_PARALLEL_CH)
    target_compile_definitions(glint_static PUBLIC GLINT_PARALLEL_CHANNELS)
    target_compile_definitions(glint_shared PUBLIC GLINT_PARALLEL_CHANNELS)
    target_link_libraries(glint_static PUBLIC Threads::Threads)
    target_link_libraries(glint_shared PUBLIC Threads::Threads)
endif()

if(GLINT_MODE STREQUAL "fixed")
    target_compile_definitions(glint_static PUBLIC GLINT_FIXED_POINT GLINT_SMALL_BUFFERS GLINT_SMALL_POW34)
    target_compile_definitions(glint_shared PUBLIC GLINT_FIXED_POINT GLINT_SMALL_BUFFERS GLINT_SMALL_POW34)
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
