#pragma once

#include <cstddef>

#include "aeronet/compiler-config.hpp"

// AERONET_ASAN_ENABLED is 1 when the translation unit is instrumented with AddressSanitizer, 0 otherwise.
// gcc and MSVC define __SANITIZE_ADDRESS__, clang exposes __has_feature(address_sanitizer).
#if defined(__SANITIZE_ADDRESS__)
#define AERONET_ASAN_ENABLED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define AERONET_ASAN_ENABLED 1
#endif
#endif

#ifndef AERONET_ASAN_ENABLED
#define AERONET_ASAN_ENABLED 0
#endif

#if AERONET_ASAN_ENABLED
#include <sanitizer/asan_interface.h>
#endif

namespace aeronet {

// Manual AddressSanitizer poisoning, for memory that our own allocators (object pools, buffer caches) keep for reuse.
// ASan only tracks malloc / free boundaries, so memory that is logically freed but recycled would otherwise stay
// addressable and hide use-after-free bugs. Accessing poisoned memory is reported as 'use-after-poison'.
//
// ASan tracks addressability with an 8-byte granularity, and can only record that the first k bytes of a granule are
// addressable. Poisoning is thus exact for whole granules, and for the tail of a granule whose prefix stays
// addressable (bump allocators). Otherwise, the partially covered granule may stay addressable.
// All functions are no-ops when ASan is disabled, and always inlined so that they do not cost a call, even at -O0.

// Marks [addr, addr + size) as unaddressable.
AERONET_ALWAYS_INLINE void AsanPoison([[maybe_unused]] const void* addr, [[maybe_unused]] std::size_t size) noexcept {
#if AERONET_ASAN_ENABLED
  __asan_poison_memory_region(addr, size);
#endif
}

// Marks [addr, addr + size) as addressable again.
AERONET_ALWAYS_INLINE void AsanUnpoison([[maybe_unused]] const void* addr, [[maybe_unused]] std::size_t size) noexcept {
#if AERONET_ASAN_ENABLED
  __asan_unpoison_memory_region(addr, size);
#endif
}

// Tells whether the byte at addr is poisoned (either by AsanPoison or by ASan itself). Always false without ASan.
[[nodiscard]] AERONET_ALWAYS_INLINE bool AsanIsPoisoned([[maybe_unused]] const void* addr) noexcept {
#if AERONET_ASAN_ENABLED
  return __asan_address_is_poisoned(addr) != 0;
#else
  return false;
#endif
}

}  // namespace aeronet
