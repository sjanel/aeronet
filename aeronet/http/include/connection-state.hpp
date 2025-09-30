#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include "raw-chars.hpp"
#include "transport.hpp"

namespace aeronet {

struct ConnectionState {
  RawChars buffer;                        // accumulated raw data
  RawChars bodyStorage;                   // decoded body lifetime
  RawChars outBuffer;                     // pending outbound bytes not yet written
  std::unique_ptr<ITransport> transport;  // set after accept (plain or TLS)
  std::chrono::steady_clock::time_point lastActivity{std::chrono::steady_clock::now()};
  // Timestamp of first byte of the current pending request headers (buffer not yet containing full CRLFCRLF).
  // Reset when a complete request head is parsed. If std::chrono::steady_clock::time_point{} (epoch) -> inactive.
  std::chrono::steady_clock::time_point headerStart;  // default epoch value means no header timing active
  uint32_t requestsServed{0};
  bool shouldClose{false};      // request to close once outBuffer drains
  bool waitingWritable{false};  // EPOLLOUT registered
  bool tlsEstablished{false};   // true once TLS handshake completed (if TLS enabled)
  bool tlsWantRead{false};      // last transport op indicated WANT_READ
  bool tlsWantWrite{false};     // last transport op indicated WANT_WRITE
  // TODO: maybe we could add closeConnection bool here
  std::string selectedAlpn;                              // negotiated ALPN protocol (if any)
  std::string negotiatedCipher;                          // negotiated TLS cipher suite (if TLS)
  std::string negotiatedVersion;                         // negotiated TLS protocol version string
  std::chrono::steady_clock::time_point handshakeStart;  // TLS handshake start time (steady clock)
};

}  // namespace aeronet