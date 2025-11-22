#pragma once

#if defined(__clang__) && defined(__clang_minor__)
#define AERONET_CLANG (__clang_major__ * 10000 + __clang_minor__ * 100 + __clang_patchlevel__)
#elif defined(__GNUC__) && defined(__GNUC_MINOR__) && defined(__GNUC_PATCHLEVEL__)
#define AERONET_GCC (__GNUC__ * 10000 + __GNUC_MINOR__ * 100 + __GNUC_PATCHLEVEL__)
#elif defined(_MSC_FULL_VER)
#define AERONET_MSVC _MSC_FULL_VER
#endif

#if defined(__GNUC__)
#define AERONET_LIKELY(x) (__builtin_expect(!!(x), 1))
#define AERONET_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#define AERONET_LIKELY(x) (!!(x))
#define AERONET_UNLIKELY(x) (!!(x))
#endif

#define AERONET_PUSH_WARNING _Pragma("GCC diagnostic push")
#define AERONET_POP_WARNING _Pragma("GCC diagnostic pop")

#define AERONET_DISABLE_WARNING_INTERNAL(warningName) #warningName
#define AERONET_DISABLE_WARNING(warningName) \
  _Pragma(AERONET_DISABLE_WARNING_INTERNAL(GCC diagnostic ignored warningName))
#ifdef AERONET_CLANG
#define AERONET_CLANG_DISABLE_WARNING(warningName) AERONET_DISABLE_WARNING(warningName)
#define AERONET_GCC_DISABLE_WARNING(warningName)
#else
#define AERONET_CLANG_DISABLE_WARNING(warningName)
#define AERONET_GCC_DISABLE_WARNING(warningName) AERONET_DISABLE_WARNING(warningName)
#endif

#ifdef AERONET_MSVC
#define AERONET_ALWAYS_INLINE __forceinline
#define AERONET_NOINLINE __declspec(noinline)
// MSVC does not implement P2647R1 yet
#define AERONET_STATIC_CONSTEXPR_IN_CONSTEXPR_FUNC constexpr
#elifdef __GNUC__
#define AERONET_ALWAYS_INLINE inline __attribute__((__always_inline__))
#define AERONET_NOINLINE __attribute__((__noinline__))
#define AERONET_STATIC_CONSTEXPR_IN_CONSTEXPR_FUNC static constexpr
#else
#define AERONET_ALWAYS_INLINE inline
#define AERONET_NOINLINE
#define AERONET_STATIC_CONSTEXPR_IN_CONSTEXPR_FUNC static constexpr
#endif

#define AERONET_STRINGIFY(x) #x
#define AERONET_VER_STRING(major, minor, patch) \
  AERONET_STRINGIFY(major) "." AERONET_STRINGIFY(minor) "." AERONET_STRINGIFY(patch)

#ifdef AERONET_CLANG
#define AERONET_COMPILER_NAME "clang"
#define AERONET_COMPILER_VERSION \
  AERONET_COMPILER_NAME " " AERONET_VER_STRING(__clang_major__, __clang_minor__, __clang_patchlevel__)
#elifdef __GNUC__
#define AERONET_COMPILER_NAME "g++"
#define AERONET_COMPILER_VERSION \
  AERONET_COMPILER_NAME " " AERONET_VER_STRING(__GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__)
#elif defined(_MSC_VER)
#define AERONET_COMPILER_NAME "MSVC"
#define AERONET_COMPILER_VERSION AERONET_COMPILER_NAME " " AERONET_STRINGIFY(_MSC_FULL_VER)
#else
#error "Unknown compiler. Only clang, gcc and MSVC are supported."
#endif

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define AERONET_ASAN_ENABLED 1
#endif
#endif

#if defined(__SANITIZE_ADDRESS__)
#define AERONET_ASAN_ENABLED 1
#endif