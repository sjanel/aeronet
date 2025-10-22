#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "aeronet/http-response-data.hpp"
#include "raw-chars.hpp"
#include "tls-info.hpp"
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

  std::size_t transportRead(std::size_t chunkSize, TransportHint& want);

  std::size_t transportWrite(std::string_view data, TransportHint& want);
  std::size_t transportWrite(const HttpResponseData& httpResponseData, TransportHint& want);

  [[nodiscard]] bool isTunneling() const noexcept { return peerFd != -1; }

  // Buffer used for tunneling raw bytes when peer is not writable.
  RawChars tunnelOutBuffer;

  RawChars inBuffer;                      // accumulated raw data
  RawChars bodyAndTrailersBuffer;         // decoded body + optional trailer headers (RFC 7230 §4.1.2)
  HttpResponseData outBuffer;             // pending outbound data not yet written
  std::unique_ptr<ITransport> transport;  // set after accept (plain or TLS)
  std::chrono::steady_clock::time_point lastActivity{std::chrono::steady_clock::now()};
  // Timestamp of first byte of the current pending request headers (buffer not yet containing full CRLFCRLF).
  // Reset when a complete request head is parsed. If std::chrono::steady_clock::time_point{} (epoch) -> inactive.
  std::chrono::steady_clock::time_point headerStart;  // default epoch value means no header timing active
  // Tunnel support: when a connection is acting as a tunnel endpoint, peerFd holds the
  // file descriptor of the other side (upstream or client).
  int peerFd{-1};
  uint32_t requestsServed{0};
  // Position where trailer headers start in bodyAndTrailersBuffer (0 if no trailers).
  // Trailers occupy [trailerStartPos, bodyAndTrailersBuffer.size()).
  std::size_t trailerStartPos{0};
  // Connection close lifecycle.
  enum class CloseMode : uint8_t { None, DrainThenClose, Immediate };

  CloseMode closeMode{CloseMode::None};
  bool waitingWritable{false};  // EPOLLOUT registered
  bool tlsEstablished{false};   // true once TLS handshake completed (if TLS enabled)
  // Tunnel state: true when peerFd != -1. Use accessor isTunneling() to query.
  // True when a non-blocking connect() was issued and completion is pending (EPOLLOUT will signal).
  bool connectPending{false};
  TLSInfo tlsInfo;
  std::chrono::steady_clock::time_point handshakeStart;  // TLS handshake start time (steady clock)
};

}  // namespace aeronet