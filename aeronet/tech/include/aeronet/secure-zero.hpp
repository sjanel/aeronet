#pragma once

#include <atomic>
#include <cstddef>

namespace aeronet {

// Overwrite sensitive memory through volatile stores so the compiler cannot remove the scrub as a dead write.
// The signal fence prevents surrounding memory operations from being reordered across the scrub.
inline void SecureZero(void* ptr, std::size_t size) noexcept {
  for (auto* bytes = static_cast<volatile unsigned char*>(ptr); size != 0; --size) {
    *bytes++ = 0;
  }
  std::atomic_signal_fence(std::memory_order_seq_cst);
}

}  // namespace aeronet
