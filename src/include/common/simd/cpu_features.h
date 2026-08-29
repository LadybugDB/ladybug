#pragma once

#include <cstdint>

#if defined(_MSC_VER) && (defined(__x86_64__) || defined(_M_X64))
#include <intrin.h>
#endif

namespace lbug {
namespace common {
namespace simd {

// Runtime detection of AVX2 support (CPU + OS/XSAVE enablement), callable from code that is not
// itself compiled with AVX2 enabled. This is implemented with direct CPUID/XGETBV instead of
// __builtin_cpu_supports() because Apple Clang lowers that builtin to a reference to the
// __cpu_model symbol, which Apple's toolchain does not provide for x86_64 static linking
// (https://github.com/LadybugDB/ladybug/issues/848).
inline bool detectAVX2Support() {
#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
    int registers[4] = {};
    __cpuid(registers, 0);
    if (registers[0] < 7) {
        return false;
    }
    __cpuidex(registers, 1, 0);
    constexpr int OSXSAVE = 1 << 27;
    constexpr int AVX = 1 << 28;
    if ((registers[2] & (OSXSAVE | AVX)) != (OSXSAVE | AVX) || (_xgetbv(0) & 0x6) != 0x6) {
        return false;
    }
    __cpuidex(registers, 7, 0);
    constexpr int AVX2 = 1 << 5;
    return (registers[1] & AVX2) != 0;
#elif defined(__GNUC__) || defined(__clang__)
    uint32_t maxLeaf = 0, ebx = 0, ecx = 0, edx = 0;
    __asm__ __volatile__("cpuid" : "=a"(maxLeaf), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0), "c"(0));
    if (maxLeaf < 7) {
        return false;
    }
    // CPUID leaf 1: check OSXSAVE and AVX.
    uint32_t eax = 0;
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    constexpr uint32_t OSXSAVE = 1u << 27;
    constexpr uint32_t AVX = 1u << 28;
    if ((ecx & (OSXSAVE | AVX)) != (OSXSAVE | AVX)) {
        return false;
    }
    // XGETBV(0): the OS must have enabled XMM (SSE) and YMM (AVX) state.
    uint32_t xcr0Lo = 0, xcr0Hi = 0;
    __asm__ __volatile__("xgetbv" : "=a"(xcr0Lo), "=d"(xcr0Hi) : "c"(0));
    constexpr uint32_t XSTATE_SSE = 1u << 1;
    constexpr uint32_t XSTATE_AVX = 1u << 2;
    const uint64_t xcr0 = (static_cast<uint64_t>(xcr0Hi) << 32) | xcr0Lo;
    if ((xcr0 & (XSTATE_SSE | XSTATE_AVX)) != (XSTATE_SSE | XSTATE_AVX)) {
        return false;
    }
    // CPUID leaf 7, subleaf 0: check AVX2.
    __asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(7), "c"(0));
    constexpr uint32_t AVX2 = 1u << 5;
    return (ebx & AVX2) != 0;
#else
    return false;
#endif
#else
    return false;
#endif
}

// Public entry point. CPUID results never change at runtime, so detect once and cache.
inline bool cpuSupportsAVX2() {
    static const bool supported = detectAVX2Support();
    return supported;
}

} // namespace simd
} // namespace common
} // namespace lbug
