#pragma once

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

namespace aeronet {

#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
inline bool HasAvx2() { return __builtin_cpu_supports("avx2"); }
inline bool HasSsse3() { return __builtin_cpu_supports("ssse3"); }
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
// cpuid is slow (and traps to the hypervisor in a VM): query it once.
inline bool HasAvx2() {
  static const bool kHasAvx2 = [] {
    int cpuInfo[4];
    __cpuidex(cpuInfo, 7, 0);
    return (cpuInfo[1] & (1U << 5U)) != 0;
  }();
  return kHasAvx2;
}
inline bool HasSsse3() {
  static const bool kHasSsse3 = [] {
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    return (cpuInfo[2] & (1U << 9U)) != 0;
  }();
  return kHasSsse3;
}
#else
inline bool HasAvx2() { return false; }
inline bool HasSsse3() { return false; }
#endif

}  // namespace aeronet
