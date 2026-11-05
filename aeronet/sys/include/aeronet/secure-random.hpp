#pragma once

#include <cstddef>
#include <span>

namespace aeronet {

// Fills 'out' with bytes of the cryptographically secure generator of the operating system: getrandom() on Linux,
// arc4random_buf() on macOS, BCryptGenRandom() on Windows. A system call (on Linux): draw the bytes by batches.
// Throws std::system_error on failure.
void FillSecureRandomBytes(std::span<std::byte> out);

}  // namespace aeronet
