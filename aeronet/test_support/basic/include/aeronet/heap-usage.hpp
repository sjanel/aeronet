#pragma once

#include <cstddef>
#include <optional>

namespace aeronet::test {

// Bytes currently allocated by the process (in use, not the free memory kept by the allocator), from the allocator
// statistics: glibc, macOS, and the ASAN / TSAN runtimes. std::nullopt where there are none (musl, Windows).
[[nodiscard]] std::optional<std::size_t> AllocatedHeapBytes();

}  // namespace aeronet::test
