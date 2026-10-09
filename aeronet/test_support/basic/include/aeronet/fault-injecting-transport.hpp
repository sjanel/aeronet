#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "aeronet/fault-policy.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/transport.hpp"

namespace aeronet::test {

/// Decorator that wraps an existing Transport and applies a FaultPolicy to all I/O operations.
/// Used with real sockets (e.g., socketpair + event loop) to inject transport-level faults
/// while keeping the event loop and full server stack operational.
class FaultInjectingTransport final : public TransportBackend<FaultInjectingTransport> {
 public:
  FaultInjectingTransport(Transport inner, FaultPolicy policy);

  TransportResult read(char* buf, std::size_t len);
  TransportResult write(std::string_view data);
  TransportResult write(std::string_view firstBuf, std::string_view secondBuf);

  [[nodiscard]] bool handshakeDone() const noexcept { return _inner.handshakeDone(); }
  [[nodiscard]] bool hasPendingReadData() const noexcept {
    return !_readAheadBuffer.empty() || _readAheadStopPending || _inner.hasPendingReadData();
  }

  /// Mutable access to fault policy for mid-test reconfiguration.
  FaultPolicy& faultPolicy() { return _policy; }
  [[nodiscard]] const FaultPolicy& faultPolicy() const { return _policy; }

  /// Access the wrapped transport.
  [[nodiscard]] Transport& inner() { return _inner; }

  /// Total bytes successfully read through this transport.
  [[nodiscard]] std::size_t totalBytesRead() const { return _totalBytesRead; }

  /// Total bytes successfully written through this transport.
  [[nodiscard]] std::size_t totalBytesWritten() const { return _totalBytesWritten; }

 private:
  // Read of FaultPolicy::readAhead: drains the inner transport into _readAheadBuffer, then serves at most len bytes.
  TransportResult readAhead(char* buf, std::size_t len);

  FaultPolicy _policy;
  std::size_t _totalBytesRead{0};
  std::size_t _totalBytesWritten{0};
  Transport _inner;
  RawChars _readAheadBuffer;
  uint32_t _readCallCount{0};
  uint32_t _writeCallCount{0};
  // True when the read-ahead stopped on an EOF or an error, that the next read reports once the buffer is served (as
  // a TLS transport reports a peer close_notify read ahead behind the data).
  bool _readAheadStopPending{false};
};

}  // namespace aeronet::test
