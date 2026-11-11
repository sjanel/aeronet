#include "aeronet/heap-usage.hpp"

#ifdef AERONET_MACOS
#include <malloc/malloc.h>
#elifdef AERONET_LINUX
#include <malloc.h>  // mallinfo2 and, through <features.h>, __GLIBC__
#endif

#include <cstddef>
#include <optional>

// Exported by the ASAN and TSAN runtimes of gcc and clang. Declared here: gcc does not install
// <sanitizer/allocator_interface.h>.
extern "C" std::size_t __sanitizer_get_current_allocated_bytes();  // NOLINT(bugprone-reserved-identifier)

namespace aeronet::test {

namespace {

// gcc and MSVC define __SANITIZE_ADDRESS__ / __SANITIZE_THREAD__, clang exposes __has_feature.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitizerAllocator = true;
#elifdef __has_feature
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kSanitizerAllocator = true;
#else
constexpr bool kSanitizerAllocator = false;
#endif
#else
constexpr bool kSanitizerAllocator = false;
#endif

}  // namespace

std::optional<std::size_t> AllocatedHeapBytes() {
  if constexpr (kSanitizerAllocator) {
    // The sanitizer runtime replaces the allocator: the libc statistics do not see its allocations.
    return __sanitizer_get_current_allocated_bytes();
  }
#ifdef AERONET_MACOS
  malloc_statistics_t stats{};
  ::malloc_zone_statistics(nullptr, &stats);  // nullptr: the sum of all the zones
  return stats.size_in_use;
#elifdef __GLIBC__
  const struct mallinfo2 info = ::mallinfo2();  // all the arenas
  return info.uordblks + info.hblkhd;           // in use in the arenas + the mmapped chunks
#else
  return std::nullopt;
#endif
}

}  // namespace aeronet::test
