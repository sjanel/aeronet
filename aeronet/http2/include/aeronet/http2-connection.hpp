#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <string_view>

#include "aeronet/flat-hash-map.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/hpack.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/http2-stream.hpp"
#include "aeronet/raw-bytes.hpp"

namespace aeronet::http2 {

/// HTTP/2 connection state.
enum class ConnectionState : uint8_t {
  /// Waiting for connection preface (client magic string).
  AwaitingPreface,
  /// Connection preface received, waiting for initial SETTINGS.
  AwaitingSettings,
  /// Connection established, normal operation.
  Open,
  /// GOAWAY sent, no new streams, draining existing streams.
  GoAwaySent,
  /// GOAWAY received, no new streams, processing existing streams.
  GoAwayReceived,
  /// Connection closed.
  Closed
};

/// Callback for handling stream data.
using DataCallback = std::function<void(uint32_t streamId, std::span<const std::byte> data, bool endStream)>;

/// Callback for stream events.
using StreamEventCallback = std::function<void(uint32_t streamId)>;

/// HTTP/2 connection manager (RFC 9113).
///
/// Manages the HTTP/2 protocol state machine for a single connection including:
/// - Connection preface and SETTINGS exchange
/// - Frame parsing and dispatching
/// - Stream lifecycle management
/// - Flow control (connection and stream level)
/// - HPACK compression state
/// - Error handling and GOAWAY
///
/// Thread safety: NOT thread-safe. The connection is managed on the
/// single-threaded event loop.
class Http2Connection {
 public:
  /// Result of processing incoming data.
  struct ProcessResult {
    enum class Action : uint8_t {
      Continue,     ///< More data needed or can continue
      OutputReady,  ///< Output buffer has data to send
      Error,        ///< Protocol error, connection should be closed
      GoAway,       ///< GOAWAY sent/received, begin drain
      Closed        ///< Connection is closed
    };

    Action action{Action::Continue};
    ErrorCode errorCode{ErrorCode::NoError};
    std::size_t bytesConsumed{0};
    const char* errorMessage;
  };

  /// Create a new HTTP/2 connection with the specified configuration.
  /// @param config HTTP/2 configuration
  /// @param isServer True if this is the server side of the connection
  explicit Http2Connection(const Http2Config& config, bool isServer = true);

  // ============================
  // Connection lifecycle
  // ============================

  /// Get the current connection state.
  [[nodiscard]] ConnectionState state() const noexcept { return _state; }

  /// Check if the connection is open for new streams.
  [[nodiscard]] bool isOpen() const noexcept { return _state == ConnectionState::Open; }

  /// Check if the connection can accept new streams.
  [[nodiscard]] bool canCreateStreams() const noexcept {
    return _state == ConnectionState::Open && _activeStreamCount < _peerSettings.maxConcurrentStreams;
  }

  /// Process incoming data from the transport.
  /// @param data Incoming bytes from the socket
  /// @return ProcessResult indicating next action and bytes consumed
  [[nodiscard]] ProcessResult processInput(std::span<const std::byte> data);

  /// Get pending output data to be written to the transport.
  /// @return View of the output buffer (valid until next processInput or write operation)
  [[nodiscard]] std::span<const std::byte> getPendingOutput() const noexcept {
    return {_outputBuffer.begin() + _outputWritePos, _outputBuffer.end()};
  }

  /// Notify that output was successfully written to the transport.
  /// @param bytesWritten Number of bytes written
  void onOutputWritten(std::size_t bytesWritten);

  /// Check if there's pending output to write.
  [[nodiscard]] bool hasPendingOutput() const noexcept { return _outputWritePos < _outputBuffer.size(); }

  /// Initiate graceful shutdown by sending GOAWAY.
  /// @param errorCode Error code to include in GOAWAY (default: NO_ERROR)
  /// @param debugData Optional debug data string
  void initiateGoAway(ErrorCode errorCode = ErrorCode::NoError, std::string_view debugData = {});

  /// Send the server connection preface (SETTINGS frame) immediately.
  /// For HTTP/2 over TLS (ALPN "h2"), the server must send its SETTINGS frame
  /// immediately after the TLS handshake completes, without waiting for the client preface.
  /// This differs from h2c (cleartext) where the server waits for the client preface first.
  /// Call this method once immediately after creating the connection for TLS ALPN "h2".
  /// @note This is a no-op if SETTINGS have already been sent.
  void sendServerPreface();

  /// Send the client connection preface (magic string + SETTINGS frame).
  /// For HTTP/2 clients, the connection preface consists of:
  /// 1. The magic string "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"
  /// 2. A SETTINGS frame
  /// Call this method once immediately after creating a client-side connection.
  /// @note This is a no-op if SETTINGS have already been sent.
  void sendClientPreface();

  // ============================
  // Stream management
  // ============================

  /// Get a stream by ID.
  /// @return Pointer to the stream, or nullptr if not found
  [[nodiscard]] Http2Stream* getStream(uint32_t streamId) noexcept;

  /// Get the number of active streams.
  [[nodiscard]] uint32_t activeStreamCount() const noexcept { return _activeStreamCount; }

  /// Get the highest stream ID received from the peer.
  [[nodiscard]] uint32_t lastPeerStreamId() const noexcept { return _lastPeerStreamId; }

  /// Get the highest stream ID we've created.
  [[nodiscard]] uint32_t lastLocalStreamId() const noexcept { return _lastLocalStreamId; }

  // ============================
  // Frame sending
  // ============================

  /// Send HEADERS frame to start a new request/response.
  /// @param streamId Stream ID (must be valid for new stream creation)
  /// @param headers Headers to send
  /// @param endStream True to set END_STREAM flag
  /// @return ErrorCode if the operation failed, NoError otherwise
  [[nodiscard]] ErrorCode sendHeaders(uint32_t streamId, http::StatusCode statusCode, HeadersView headersView,
                                      bool endStream);

  /// Send DATA frame.
  /// @param streamId Stream ID
  /// @param data Data to send
  /// @param endStream True to set END_STREAM flag
  /// @return ErrorCode if the operation failed, NoError otherwise
  [[nodiscard]] ErrorCode sendData(uint32_t streamId, std::span<const std::byte> data, bool endStream);

  /// Send RST_STREAM frame.
  /// @param streamId Stream ID
  /// @param errorCode Error code for the reset
  void sendRstStream(uint32_t streamId, ErrorCode errorCode);

  /// Send PING frame.
  void sendPing(PingFrame pingFrame);

  /// Send WINDOW_UPDATE frame.
  /// @param streamId Stream ID (0 for connection-level)
  /// @param increment Window size increment
  void sendWindowUpdate(uint32_t streamId, uint32_t increment);

  // ============================
  // Settings
  // ============================

  /// Get our (local) settings.
  [[nodiscard]] const Http2Config& localSettings() const noexcept { return _localSettings; }

  /// Get peer's settings.
  struct PeerSettings {
    uint32_t headerTableSize{4096};
    bool enablePush{true};
    uint32_t maxConcurrentStreams{100};
    uint32_t initialWindowSize{65535};
    uint32_t maxFrameSize{16384};
    uint32_t maxHeaderListSize{~static_cast<uint32_t>(0)};
  };

  [[nodiscard]] const PeerSettings& peerSettings() const noexcept { return _peerSettings; }

  // ============================
  // Flow control
  // ============================

  /// Get the connection-level send window.
  [[nodiscard]] int32_t connectionSendWindow() const noexcept { return _connectionSendWindow; }

  /// Get the connection-level receive window.
  [[nodiscard]] int32_t connectionRecvWindow() const noexcept { return _connectionRecvWindow; }

  // ============================
  // Callbacks
  // ============================

  using OnHeadersCb = std::function<void(uint32_t streamId, const HeadersViewMap& headers, bool endStream)>;
  using GoAwayCb = std::function<void(uint32_t lastStreamId, ErrorCode errorCode, std::string_view debugData)>;
  using OnStreamCb = std::function<void(uint32_t streamId, ErrorCode errorCode)>;

  /// Alternative callback that receives decoded headers as an owned vector.
  /// This avoids the callback-of-callback pattern and is simpler for consumers.
  void setOnHeadersDecoded(OnHeadersCb cb) { _onHeadersDecoded = std::move(cb); }

  /// Set callback for when data is received on a stream.
  void setOnData(DataCallback callback) { _onData = std::move(callback); }

  /// Set callback for when a stream is reset.
  void setOnStreamReset(OnStreamCb callback) { _onStreamReset = std::move(callback); }

  /// Set callback for when a stream is closed.
  void setOnStreamClosed(StreamEventCallback callback) { _onStreamClosed = std::move(callback); }

  /// Set callback for GOAWAY received.
  void setOnGoAway(GoAwayCb callback) { _onGoAway = std::move(callback); }

 private:
  using StreamsMap = flat_hash_map<uint32_t, Http2Stream>;

  // ============================
  // Frame processing
  // ============================

  ProcessResult processPreface(std::span<const std::byte> data);
  ProcessResult processFrames(std::span<const std::byte> data);
  ProcessResult processFrame(FrameHeader header, std::span<const std::byte> payload);

  ProcessResult handleDataFrame(FrameHeader header, std::span<const std::byte> payload);
  ProcessResult handleHeadersFrame(FrameHeader header, std::span<const std::byte> payload);
  ProcessResult handlePriorityFrame(FrameHeader header, std::span<const std::byte> payload);
  ProcessResult handleRstStreamFrame(FrameHeader header, std::span<const std::byte> payload);
  ProcessResult handleSettingsFrame(FrameHeader header, std::span<const std::byte> payload);
  ProcessResult handlePingFrame(FrameHeader header, std::span<const std::byte> payload);
  ProcessResult handleGoAwayFrame(FrameHeader header, std::span<const std::byte> payload);
  ProcessResult handleWindowUpdateFrame(FrameHeader header, std::span<const std::byte> payload);
  ProcessResult handleContinuationFrame(FrameHeader header, std::span<const std::byte> payload);

  // ============================
  // Stream management
  // ============================
  void closeStream(StreamsMap::iterator it, ErrorCode errorCode = ErrorCode::NoError);
  void pruneClosedStreams();

  // ============================
  // HPACK
  // ============================

  void encodeHeaders(uint32_t streamId, http::StatusCode statusCode, HeadersView headersView, bool endStream,
                     bool endHeaders);

  /// Decode an HPACK header block and deliver decoded headers via `setOnHeadersDecoded`.
  /// Returns `CompressionError` if decoding fails, `NoError` otherwise.
  ErrorCode decodeAndEmitHeaders(uint32_t streamId, std::span<const std::byte> headerBlock, bool endStream);

  // ============================
  // Output helpers
  // ============================

  void sendSettings();
  void sendSettingsAck();

  // ============================
  // Error handling
  // ============================

  ProcessResult connectionError(ErrorCode code, const char* message);
  ProcessResult streamError(uint32_t streamId, ErrorCode code, const char* message);

  // ============================
  // Member variables
  // ============================

  Http2Config _localSettings;
  PeerSettings _peerSettings;

  // Stream management
  StreamsMap _streams;
  std::deque<uint32_t> _closedStreamsFifo;
  uint32_t _headerBlockStreamId{0};
  uint32_t _activeStreamCount{0};
  uint32_t _lastPeerStreamId{0};
  uint32_t _lastLocalStreamId{0};
  uint32_t _goAwayLastStreamId{UINT32_MAX};

  // Flow control
  int32_t _connectionSendWindow;
  int32_t _connectionRecvWindow;

  // HPACK state
  HpackEncoder _hpackEncoder;
  HpackDecoder _hpackDecoder;

  // Header block accumulation (for CONTINUATION frames)
  RawBytes _headerBlockBuffer;

  // Output buffer
  RawBytes _outputBuffer;
  std::size_t _outputWritePos{0};

  // Callbacks
  OnHeadersCb _onHeadersDecoded;
  DataCallback _onData;
  OnStreamCb _onStreamReset;
  StreamEventCallback _onStreamClosed;
  GoAwayCb _onGoAway;

  ConnectionState _state{ConnectionState::AwaitingPreface};

  // Settings acknowledgment tracking
  bool _settingsSent{false};
  bool _settingsAckReceived{false};
  bool _isServer;
  bool _expectingContinuation{false};
  bool _headerBlockEndStream{false};
};

}  // namespace aeronet::http2
