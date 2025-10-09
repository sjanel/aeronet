#pragma once

#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "raw-chars.hpp"
#include "transport.hpp"

namespace aeronet {

struct ConnectionState {
  // Request to close immediately (abort outstanding buffered writes).
  void requestImmediateClose() { closeMode = CloseMode::Immediate; }

  // Request to close after draining currently buffered writes (graceful half-close semantics).
  void requestDrainAndClose() {
    if (closeMode == CloseMode::None) {
      closeMode = CloseMode::DrainThenClose;
    }
  }

  [[nodiscard]] bool isImmediateCloseRequested() const noexcept { return closeMode == CloseMode::Immediate; }
  [[nodiscard]] bool isDrainCloseRequested() const noexcept { return closeMode == CloseMode::DrainThenClose; }
  [[nodiscard]] bool isAnyCloseRequested() const noexcept { return closeMode != CloseMode::None; }

  ssize_t transportRead(std::size_t chunkSize, bool& wantRead, bool& wantWrite);
  ssize_t transportWrite(std::string_view data, bool& wantRead, bool& wantWrite) const;

  RawChars buffer;                        // accumulated raw data
  RawChars bodyBuffer;                    // decoded body lifetime
  RawChars outBuffer;                     // pending outbound bytes not yet written
  std::unique_ptr<ITransport> transport;  // set after accept (plain or TLS)
  std::chrono::steady_clock::time_point lastActivity{std::chrono::steady_clock::now()};
  // Timestamp of first byte of the current pending request headers (buffer not yet containing full CRLFCRLF).
  // Reset when a complete request head is parsed. If std::chrono::steady_clock::time_point{} (epoch) -> inactive.
  std::chrono::steady_clock::time_point headerStart;  // default epoch value means no header timing active
  uint32_t requestsServed{0};
  // Connection close lifecycle.
  enum class CloseMode : uint8_t { None, DrainThenClose, Immediate };

  CloseMode closeMode{CloseMode::None};
  bool waitingWritable{false};                           // EPOLLOUT registered
  bool tlsEstablished{false};                            // true once TLS handshake completed (if TLS enabled)
  bool tlsWantRead{false};                               // last transport op indicated WANT_READ
  bool tlsWantWrite{false};                              // last transport op indicated WANT_WRITE
  std::string selectedAlpn;                              // negotiated ALPN protocol (if any)
  std::string negotiatedCipher;                          // negotiated TLS cipher suite (if TLS)
  std::string negotiatedVersion;                         // negotiated TLS protocol version string
  std::chrono::steady_clock::time_point handshakeStart;  // TLS handshake start time (steady clock)
};

}  // namespace aeronet