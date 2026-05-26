#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "aeronet/fault-policy.hpp"
#include "aeronet/transport.hpp"

namespace aeronet::test {

/// Decorator that wraps an existing ITransport and applies a FaultPolicy to all I/O operations.
/// Used with real sockets (e.g., socketpair + event loop) to inject transport-level faults
/// while keeping the event loop and full server stack operational.
class FaultInjectingTransport final : public ITransport {
 public:
  FaultInjectingTransport(std::unique_ptr<ITransport> inner, FaultPolicy policy);

  TransportResult read(char* buf, std::size_t len) override;
  TransportResult write(std::string_view data) override;
  TransportResult write(std::string_view firstBuf, std::string_view secondBuf) override;

  [[nodiscard]] bool handshakeDone() const noexcept override { return _inner->handshakeDone(); }
  [[nodiscard]] bool hasPendingReadData() const noexcept override { return _inner->hasPendingReadData(); }

  /// Mutable access to fault policy for mid-test reconfiguration.
  FaultPolicy& faultPolicy() { return _policy; }
  [[nodiscard]] const FaultPolicy& faultPolicy() const { return _policy; }

  /// Access the wrapped transport.
  [[nodiscard]] ITransport& inner() { return *_inner; }

  /// Total bytes successfully read through this transport.
  [[nodiscard]] std::size_t totalBytesRead() const { return _totalBytesRead; }

  /// Total bytes successfully written through this transport.
  [[nodiscard]] std::size_t totalBytesWritten() const { return _totalBytesWritten; }

 private:
  std::unique_ptr<ITransport> _inner;
  FaultPolicy _policy;

  std::size_t _totalBytesRead{0};
  std::size_t _totalBytesWritten{0};
  uint32_t _readCallCount{0};
  uint32_t _writeCallCount{0};
};

}  // namespace aeronet::test
