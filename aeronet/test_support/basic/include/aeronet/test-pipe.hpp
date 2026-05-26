#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet::test {

/// Bidirectional in-memory byte channel for TestTransport.
/// Simulates a network connection between a test client and the server's protocol handler.
/// Not thread-safe: designed for single-threaded deterministic tests.
class TestPipe {
 public:
  // --- Client-side API (test code pushes data toward server) ---

  /// Push data from the test client toward the server transport.
  void pushToServer(std::string_view data) { _clientToServer.append(data); }

  /// Pull data that the server has written (responses).
  /// Returns up to maxBytes (0 = all available).
  std::string pullFromServer(std::size_t maxBytes = 0);

  /// Bytes available for the server to read.
  [[nodiscard]] std::size_t serverReadAvailable() const { return _clientToServer.size(); }

  /// Bytes available for the client to read (server output).
  [[nodiscard]] std::size_t clientReadAvailable() const { return _serverToClient.size(); }

  // --- Server-side API (TestTransport uses these) ---

  /// Read bytes destined for the server (from client). Removes consumed bytes.
  /// Returns up to maxBytes from the front. Returns empty if no data available.
  std::string_view serverRead(std::size_t maxBytes);

  /// Acknowledge consumption of bytes previously returned by serverRead().
  /// Must be called after processing the data from serverRead().
  void serverConsume(std::size_t bytes) { _clientToServer.erase_front(bytes); }

  /// Write bytes from the server (toward client).
  void serverWrite(std::string_view data) { _serverToClient.append(data); }

  // --- Connection state simulation ---

  /// Simulate orderly close from client side (EOF on server read).
  void closeClientEnd() { _clientClosed = true; }

  /// Simulate orderly close from server side (EOF on client read).
  void closeServerEnd() { _serverClosed = true; }

  /// Simulate connection reset from client side.
  void resetClientEnd() { _clientReset = true; }

  /// Simulate connection reset from server side.
  void resetServerEnd() { _serverReset = true; }

  [[nodiscard]] bool isClientClosed() const { return _clientClosed; }
  [[nodiscard]] bool isServerClosed() const { return _serverClosed; }
  [[nodiscard]] bool isClientReset() const { return _clientReset; }
  [[nodiscard]] bool isServerReset() const { return _serverReset; }

 private:
  RawChars _clientToServer;  // data flowing from test client → server
  RawChars _serverToClient;  // data flowing from server → test client

  bool _clientClosed{false};
  bool _serverClosed{false};
  bool _clientReset{false};
  bool _serverReset{false};
};

}  // namespace aeronet::test
