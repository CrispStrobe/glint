// glint - SIMD runtime dispatch
// MIT License - Clean-room implementation

#ifndef GLINT_SIMD_HPP
#define GLINT_SIMD_HPP

#include "glint/glint.h"

namespace glint {

// Runtime SIMD level, set during glint_create() based on glint_config.simd.
// Checked in hot loops to select the code path.
// Values match glint_simd enum: 0=auto, 1=avx, 2=sse2, 3=none.
inline int g_simd_level = 0;  // 0 = not yet initialized

// Detect best available SIMD at runtime
inline int detect_simd() {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    // On x86, SSE2 is always available on x86-64
    int level = GLINT_SIMD_SSE2;
#if defined(__GNUC__) || defined(__clang__)
    // Use __builtin_cpu_supports for runtime AVX detection
    if (__builtin_cpu_supports("avx2") || __builtin_cpu_supports("avx"))
        level = GLINT_SIMD_AVX;
#elif defined(_MSC_VER)
    // MSVC: use __cpuid
    int cpuinfo[4];
    __cpuidex(cpuinfo, 7, 0);
    if (cpuinfo[1] & (1 << 5))  // AVX2 bit in EBX
        level = GLINT_SIMD_AVX;
#endif
    return level;
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    // AArch64 NEON
    return GLINT_SIMD_NEON;
#else
    // Non-x86, non-ARM: no SIMD
    return GLINT_SIMD_NONE;
#endif
}

// Initialize SIMD level from config
inline void init_simd(glint_simd requested) {
    if (requested == GLINT_SIMD_AUTO)
        g_simd_level = detect_simd();
    else
        g_simd_level = requested;
}

} // namespace glint

#endif // GLINT_SIMD_HPP
