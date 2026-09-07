#pragma once

#include <cstddef>
#include <cstdint>

namespace aeronet {

// Indicates what the transport layer needs to proceed after a non-blocking I/O operation returns EAGAIN/WANT.
enum class TransportHint : uint8_t {
  None,        // No special action needed (operation completed or fatal error)
  ReadReady,   // Need socket readable before operation can proceed (SSL_ERROR_WANT_READ)
  WriteReady,  // Need socket writable before operation can proceed (SSL_ERROR_WANT_WRITE)
  Error,
};

struct TransportResult {
  std::size_t bytesProcessed{0};            // bytes read for read operations, or written for write operations
  TransportHint want{TransportHint::None};  // socket readiness needed before the operation can proceed
};

enum class TransportKind : uint8_t { Empty, Plain, Tls, Custom };

}  // namespace aeronet