#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "aeronet/http-message-data.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

struct ConnectionState;

/// Protocol type identifier for runtime protocol switching.
/// Used after successful Upgrade (101) or ALPN negotiation.
enum class ProtocolType : uint8_t {
  Http11,     // HTTP/1.1 (default)
  WebSocket,  // WebSocket (RFC 6455)
  Http2,      // HTTP/2 (RFC 9113, formerly RFC 7540)
};

/// Result of processing incoming data by a protocol handler.
struct ProtocolProcessResult {
  enum class Action : uint8_t {
    Continue,        // More data needed or processing can continue
    ResponseReady,   // A response/frame is ready to be sent
    Upgrade,         // Protocol upgrade requested (101 Switching Protocols)
    Close,           // Connection should be closed gracefully
    CloseImmediate,  // Connection should be closed immediately (protocol error)
  };

  Action action{Action::Continue};
  std::size_t bytesConsumed{0};  // Bytes consumed from input buffer
};

/// Base interface for protocol handlers.
///
/// This abstraction enables the server to support multiple protocols (HTTP/1.1,
/// WebSocket, HTTP/2) through a common interface. Each protocol implementation
/// handles its own framing, parsing, and response generation.
///
/// Lifecycle:
///   1. Connection accepted -> HTTP/1.1 handler installed (default)
///   2. Client requests upgrade (Upgrade header or ALPN) -> switchProtocol() called
///   3. New protocol handler processes subsequent data
///
/// Thread safety:
///   Protocol handlers are not thread-safe by design; they execute on the
///   server's single-threaded event loop.
class IProtocolHandler {
 public:
  virtual ~IProtocolHandler() = default;

  /// Returns the protocol type this handler implements.
  [[nodiscard]] virtual ProtocolType type() const noexcept = 0;

  /// Process incoming data from the connection.
  ///
  /// @param data   Incoming bytes from the transport layer
  /// @param state  Connection state (buffers, flags, transport)
  /// @return       Processing result indicating next action
  ///
  /// The handler should:
  ///   - Parse incoming frames/messages according to protocol rules
  ///   - Update connection state as needed
  ///   - Return how many bytes were consumed
  ///   - Indicate if a response is ready, upgrade requested, or error occurred
  [[nodiscard]] virtual ProtocolProcessResult processInput(std::span<const std::byte> data, ConnectionState& state) = 0;

  /// Get pending output data to be written to the transport.
  [[nodiscard]] virtual std::span<const std::byte> getPendingOutput() const noexcept = 0;

  /// Replace `fragments` with all ordered pending output views for gather writes.
  virtual void getPendingOutputFragments(vector<std::string_view>& fragments) const {
    fragments.clear();
    const auto pending = getPendingOutput();
    if (pending.empty()) {
      return;
    }
    fragments.reserve(1);
    fragments.emplace_back(reinterpret_cast<const char*>(pending.data()), pending.size());
  }

  /// Return the total pending output size.
  [[nodiscard]] virtual std::size_t pendingOutputSize() const noexcept { return getPendingOutput().size(); }
  /// Check if the handler has pending outbound data to write.

  [[nodiscard]] bool hasPendingOutput() const noexcept { return pendingOutputSize() != 0; }

  /// Notify the handler that output was successfully written.
  /// @param bytesWritten Number of bytes successfully written to transport
  virtual void onOutputWritten(std::size_t bytesWritten) = 0;

  /// Discard queued output when the transport can no longer make progress.
  virtual void discardPendingOutput() noexcept {
    for (;;) {
      const auto pending = getPendingOutput();
      if (pending.empty()) {
        return;
      }
      onOutputWritten(pending.size());
    }
  }

  /// Called when the underlying transport is about to be closed.
  /// Allows cleanup of protocol-specific state.
  virtual void onTransportClosing() = 0;

  /// Drains all pending output into dest, returning true if any data was transferred.
  ///
  /// The default implementation copies pending output via getPendingOutput() + onOutputWritten().
  /// Protocol handlers may override this to perform a zero-copy move of their internal buffer.
  virtual bool drainOutputBuffer(HttpMessageData& dest) {
    bool drained = false;
    for (;;) {
      const auto pending = getPendingOutput();
      if (pending.empty()) {
        return drained;
      }
      dest.append(reinterpret_cast<const char*>(pending.data()), pending.size());
      onOutputWritten(pending.size());
      drained = true;
    }
  }
};

/// Factory function type for creating protocol handlers.
/// Used when upgrading protocols or creating handlers for new connections.
using ProtocolHandlerFactory = std::unique_ptr<IProtocolHandler> (*)(ConnectionState&);

}  // namespace aeronet
