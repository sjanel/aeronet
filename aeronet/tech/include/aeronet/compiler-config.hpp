#pragma once

#include <cstddef>

// aeronet only supports 64-bit targets, see docs/getting-started/install.md#toolchain-platform
static_assert(sizeof(void*) == 8 && sizeof(std::size_t) == 8,
              "aeronet requires a 64-bit target (sizeof(void*) == sizeof(std::size_t) == 8): 32-bit targets are not "
              "supported");

#if defined(_MSC_VER) && !defined(__clang__)
#define AERONET_RESTRICT __restrict
#define AERONET_ALWAYS_INLINE __forceinline
#define AERONET_NOINLINE __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
#define AERONET_RESTRICT __restrict__
#define AERONET_ALWAYS_INLINE [[gnu::always_inline]] inline
#define AERONET_NOINLINE [[gnu::noinline]]
#else
#define AERONET_RESTRICT
#define AERONET_ALWAYS_INLINE inline
#define AERONET_NOINLINE
#endif
