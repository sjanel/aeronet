#pragma once

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
