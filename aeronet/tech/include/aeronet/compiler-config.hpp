#pragma once

#if defined(_MSC_VER) && !defined(__clang__)
#define AERONET_RESTRICT __restrict
#elif defined(__GNUC__) || defined(__clang__)
#define AERONET_RESTRICT __restrict__
#else
#define AERONET_RESTRICT
#endif