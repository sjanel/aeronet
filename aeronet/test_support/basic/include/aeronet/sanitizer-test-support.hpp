#pragma once

namespace aeronet::test {

// Whether the tests are built with ThreadSanitizer: gcc defines __SANITIZE_THREAD__, clang exposes
// __has_feature(thread_sanitizer), only usable in preprocessor conditions.
#ifdef __SANITIZE_THREAD__
inline constexpr bool kThreadSanitizer = true;
#elifdef __has_feature
#if __has_feature(thread_sanitizer)
inline constexpr bool kThreadSanitizer = true;
#else
inline constexpr bool kThreadSanitizer = false;
#endif
#else
inline constexpr bool kThreadSanitizer = false;
#endif

// Reason given by the CPU-bound single-threaded tests skipped under ThreadSanitizer: it only finds data races, and
// would make them last minutes (it instruments every memory access). The other builds, ASAN included, run them.
inline constexpr const char* kSkippedUnderThreadSanitizer =
    "single-threaded and CPU bound: nothing for ThreadSanitizer to find, run by the other builds";

}  // namespace aeronet::test
