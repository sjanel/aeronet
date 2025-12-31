#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-deflate.hpp"
#include "aeronet/websocket-frame.hpp"

namespace aeronet {

struct ConnectionState;

namespace websocket {

/// Configuration options for WebSocket connections.
struct WebSocketConfig {
  /// Maximum size of a single message (after reassembly from fragments).
  /// Set to 0 for unlimited (use with caution).
  std::size_t maxMessageSize{kDefaultMaxMessageSize};

  /// Maximum size of a single frame payload.
  std::size_t maxFrameSize{kDefaultMaxFrameSize};

  /// Close timeout to wait for close response.
  std::chrono::milliseconds closeTimeout{std::chrono::milliseconds{5000}};

  /// Deflate configuration (optional, for permessage-deflate extension).
  DeflateConfig deflateConfig;

  /// Whether this is the server side (affects masking validation).
  bool isServerSide{true};
};

/// Callbacks for WebSocket events.
/// All callbacks are invoked on the event loop thread.
struct WebSocketCallbacks {
  /// Called when a complete message (text or binary) is received.
  /// @param payload   The complete message payload (fragments reassembled)
  /// @param isBinary  True if binary message, false if text message
  /// Note: For text messages, the payload is guaranteed to be valid UTF-8.
  std::function<void(std::span<const std::byte> payload, bool isBinary)> onMessage;

  /// Called when a Ping frame is received.
  /// The handler automatically sends Pong response; this is informational.
  /// @param payload Ping payload (up to 125 bytes)
  std::function<void(std::span<const std::byte> payload)> onPing;

  /// Called when a Pong frame is received.
  /// @param payload Pong payload (up to 125 bytes)
  std::function<void(std::span<const std::byte> payload)> onPong;

  /// Called when a Close frame is received.
  /// The handler automatically handles the close handshake.
  /// @param code   Close status code
  /// @param reason Close reason string
  std::function<void(CloseCode code, std::string_view reason)> onClose;

  /// Called when a protocol error occurs.
  /// The connection will be closed after this callback.
  /// @param code    Error close code (e.g., ProtocolError, InvalidPayloadData)
  /// @param message Error description
  std::function<void(CloseCode code, std::string_view message)> onError;
};

/// WebSocket protocol handler implementing IProtocolHandler.
///
/// Handles WebSocket frame parsing, message assembly, control frames,
/// and close handshake according to RFC 6455.
///
/// Usage:
///   1. Create with configuration and callbacks
///   2. Install as ConnectionState.protocolHandler after successful upgrade
///   3. Server routes incoming data through processInput()
///   4. Use sendText/sendBinary/sendClose to transmit data
///
/// Thread safety: Not thread-safe (designed for single-threaded event loop).
class WebSocketHandler final : public IProtocolHandler {
 public:
  /// Create a WebSocket handler.
  /// @param config     Configuration options
  /// @param callbacks  Event callbacks (can be set later via setCallbacks)
  /// @param deflateParams Optional negotiated deflate parameters for compression
  explicit WebSocketHandler(WebSocketConfig config = {}, WebSocketCallbacks callbacks = {},
                            std::optional<DeflateNegotiatedParams> deflateParams = std::nullopt);

  // Non-copyable, movable
  WebSocketHandler(const WebSocketHandler&) = delete;
  WebSocketHandler& operator=(const WebSocketHandler&) = delete;
  WebSocketHandler(WebSocketHandler&&) noexcept;
  WebSocketHandler& operator=(WebSocketHandler&&) noexcept;

  ~WebSocketHandler() override;

  // IProtocolHandler implementation
  [[nodiscard]] ProtocolType type() const noexcept override { return ProtocolType::WebSocket; }

  [[nodiscard]] ProtocolProcessResult processInput(std::span<const std::byte> data, ConnectionState& state) override;

  [[nodiscard]] bool hasPendingOutput() const noexcept override;

  [[nodiscard]] std::span<const std::byte> getPendingOutput() override;

  void onOutputWritten(std::size_t bytesWritten) override;

  void initiateClose() override;

  void onTransportClosing() override;

  // WebSocket-specific API

  /// Set or update callbacks.
  void setCallbacks(WebSocketCallbacks callbacks);

  /// Send a text message.
  /// @param text UTF-8 encoded text message
  /// @return true if queued successfully, false if connection is closing
  bool sendText(std::string_view text);

  /// Send a binary message.
  /// @param data Binary data to send
  /// @return true if queued successfully, false if connection is closing
  bool sendBinary(std::span<const std::byte> data);

  /// Send a Ping frame.
  /// @param payload Optional payload (max 125 bytes)
  /// @return true if queued successfully, false if connection is closing
  bool sendPing(std::span<const std::byte> payload = {});

  /// Send a Pong frame (usually automatic in response to Ping).
  /// @param payload Payload to echo back
  /// @return true if queued successfully, false if connection is closing
  bool sendPong(std::span<const std::byte> payload);

  /// Initiate close handshake.
  /// @param code   Close status code
  /// @param reason Close reason (max ~123 bytes, will be truncated)
  /// @return true if close frame was sent, false if already closing
  bool sendClose(CloseCode code = CloseCode::Normal, std::string_view reason = {});

  /// Check if the connection is in closing state.
  [[nodiscard]] bool isClosing() const noexcept { return _closeState != CloseState::Open; }

  /// Check if the close handshake is complete (ready to close transport).
  [[nodiscard]] bool isCloseComplete() const noexcept { return _closeState == CloseState::Closed; }

  /// Get current configuration.
  [[nodiscard]] const WebSocketConfig& config() const noexcept { return _config; }

  /// Check if compression is enabled.
  [[nodiscard]] bool hasCompression() const noexcept { return _deflateContext != nullptr; }

  /// Get the time when close was initiated (for timeout tracking).
  /// Returns a zero duration if not in closing state.
  [[nodiscard]] std::chrono::steady_clock::time_point closeInitiatedAt() const noexcept { return _closeInitiatedAt; }

  /// Check if the close handshake has timed out.
  /// Returns true if we've been waiting for a close response longer than closeTimeout.
  [[nodiscard]] bool hasCloseTimedOut() const noexcept {
    if (_closeState != CloseState::CloseSent) {
      return false;
    }
    auto now = std::chrono::steady_clock::now();
    return (now - _closeInitiatedAt) > _config.closeTimeout;
  }

  /// Force close the connection after timeout (call after hasCloseTimedOut() returns true).
  /// This marks the close handshake as complete without receiving peer's close frame.
  void forceCloseOnTimeout() noexcept {
    if (_closeState == CloseState::CloseSent) {
      _closeState = CloseState::Closed;
    }
  }

 private:
  /// Close handshake state machine.
  enum class CloseState : uint8_t {
    Open,           // Normal operation
    CloseSent,      // We sent Close, waiting for peer's Close
    CloseReceived,  // Peer sent Close, we need to respond
    Closed          // Close handshake complete
  };

  /// State for message reassembly from fragments.
  struct MessageState {
    RawBytes buffer;              // Accumulated payload from fragments
    Opcode opcode{Opcode::Text};  // Message type from first fragment
    bool inProgress{false};       // True when receiving a fragmented message
  };

  /// Process a single complete frame.
  /// @return Processing result
  ProtocolProcessResult processFrame(const FrameParseResult& frame);

  /// Handle a data frame (Text, Binary, Continuation).
  ProtocolProcessResult handleDataFrame(const FrameHeader& header, std::span<const std::byte> payload);

  /// Handle a control frame (Ping, Pong, Close).
  ProtocolProcessResult handleControlFrame(const FrameHeader& header, std::span<const std::byte> payload);

  /// Complete a message (called when FIN=1).
  ProtocolProcessResult completeMessage();

  /// Queue a frame for sending.
  void queueFrame(Opcode opcode, std::span<const std::byte> payload, bool fin = true);

  WebSocketConfig _config;
  WebSocketCallbacks _callbacks;
  std::unique_ptr<DeflateContext> _deflateContext;          // Compression context (null if not negotiated)
  std::chrono::steady_clock::time_point _closeInitiatedAt;  // Time when close was initiated
  RawBytes _outputBuffer;                                   // Pending output data
  std::size_t _outputOffset{0};                             // Bytes already retrieved via getPendingOutput
  MessageState _message;                                    // Current message being assembled
  RawBytes _inputBuffer;                                    // Carry-over from incomplete frames
  RawBytes _compressBuffer;                                 // Temporary buffer for compression/decompression
  CloseCode _closeCode{CloseCode::Normal};
  CloseState _closeState{CloseState::Open};
  bool _messageCompressed{false};  // True if current message was received with RSV1 set
};

/// Create a WebSocket handler for server-side use.
/// Convenience factory function.
[[nodiscard]] std::unique_ptr<WebSocketHandler> CreateServerWebSocketHandler(
    WebSocketCallbacks callbacks = {}, std::size_t maxMessageSize = kDefaultMaxMessageSize);

/// Create a WebSocket handler for client-side use.
/// Convenience factory function.
[[nodiscard]] std::unique_ptr<WebSocketHandler> CreateClientWebSocketHandler(
    WebSocketCallbacks callbacks = {}, std::size_t maxMessageSize = kDefaultMaxMessageSize);

}  // namespace websocket
}  // namespace aeronet
