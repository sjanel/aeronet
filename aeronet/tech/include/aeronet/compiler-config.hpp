#pragma once

#if defined(_MSC_VER) && !defined(__clang__)
#define AERONET_RESTRICT __restrict
#define AERONET_ALWAYS_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define AERONET_RESTRICT __restrict__
#define AERONET_ALWAYS_INLINE [[gnu::always_inline]] inline
#else
#define AERONET_RESTRICT
#define AERONET_ALWAYS_INLINE inline
#endif