#pragma once

namespace aeronet {

#if (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
inline bool HasAvx2() { return __builtin_cpu_supports("avx2"); }
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
inline bool HasAvx2() {
  int cpuInfo[4];
  __cpuidex(cpuInfo, 7, 0);
  return (cpuInfo[1] & (1U << 5U)) != 0;
}
#else
inline bool HasAvx2() { return false; }
#endif

}  // namespace aeronet