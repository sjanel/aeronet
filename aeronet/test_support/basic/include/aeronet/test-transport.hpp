#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "aeronet/fault-policy.hpp"
#include "aeronet/test-pipe.hpp"
#include "aeronet/transport.hpp"

namespace aeronet::test {

/// Deterministic fault-injecting transport for protocol-level testing.
/// Implements ITransport backed by an in-memory TestPipe, with configurable
/// fault injection (partial reads/writes, EAGAIN simulation, connection resets).
///
/// Usage:
///   auto [transport, pipe] = MakeTestTransport(faultPolicy);
///   state.transport = std::move(transport);
///   pipe.pushToServer("GET / HTTP/1.1\r\n...");
///   // drive protocol processing...
///   auto response = pipe.pullFromServer();
class TestTransport final : public ITransport {
 public:
  explicit TestTransport(TestPipe& pipe, FaultPolicy policy = {});

  TransportResult read(char* buf, std::size_t len) override;

  TransportResult write(std::string_view data) override;

  [[nodiscard]] bool handshakeDone() const noexcept override { return _handshakeDone; }

  [[nodiscard]] bool hasPendingReadData() const noexcept override;

  /// Mutable access to fault policy for mid-test reconfiguration.
  FaultPolicy& faultPolicy() { return _policy; }
  [[nodiscard]] const FaultPolicy& faultPolicy() const { return _policy; }

  /// Control handshake state (for TLS simulation).
  void setHandshakeDone(bool done) { _handshakeDone = done; }

  /// Total bytes successfully read through this transport.
  [[nodiscard]] std::size_t totalBytesRead() const { return _totalBytesRead; }

  /// Total bytes successfully written through this transport.
  [[nodiscard]] std::size_t totalBytesWritten() const { return _totalBytesWritten; }

  /// Total read() calls made.
  [[nodiscard]] uint32_t readCallCount() const { return _readCallCount; }

  /// Total write() calls made.
  [[nodiscard]] uint32_t writeCallCount() const { return _writeCallCount; }

 private:
  std::size_t computeReadLimit(std::size_t available) const;

  TestPipe& _pipe;
  FaultPolicy _policy;

  std::size_t _totalBytesRead{0};
  std::size_t _totalBytesWritten{0};
  uint32_t _readCallCount{0};
  uint32_t _writeCallCount{0};
  bool _handshakeDone{true};

  // Simple xoshiro256** state for deterministic PRNG
  uint64_t _rngState[4]{};

  uint64_t nextRandom();
};

/// Result of MakeTestTransport: the transport to plug into ConnectionState,
/// and the pipe for test code to push/pull data.
struct TestTransportPair {
  std::unique_ptr<TestTransport> transport;
  TestPipe pipe;
};

/// Create a paired TestTransport + TestPipe ready for use.
TestTransportPair MakeTestTransport(FaultPolicy policy = {});

}  // namespace aeronet::test
