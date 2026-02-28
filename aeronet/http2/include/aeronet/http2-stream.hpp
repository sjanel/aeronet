#pragma once

#include <cstdint>
#include <string_view>

#include "aeronet/http2-frame-types.hpp"

namespace aeronet::http2 {

// StreamState is already defined in http2-frame-types.hpp (included via http2-frame.hpp)

/// Convert StreamState to human-readable string.
[[nodiscard]] constexpr std::string_view StreamStateName(StreamState state) noexcept {
  switch (state) {
    case StreamState::Idle:
      return "idle";
    case StreamState::ReservedLocal:
      return "reserved (local)";
    case StreamState::ReservedRemote:
      return "reserved (remote)";
    case StreamState::Open:
      return "open";
    case StreamState::HalfClosedLocal:
      return "half-closed (local)";
    case StreamState::HalfClosedRemote:
      return "half-closed (remote)";
    case StreamState::Closed:
      return "closed";
    default:
      return "unknown";
  }
}

/// HTTP/2 stream (RFC 9113 §5).
///
/// Represents a single HTTP/2 stream within a connection.
/// Manages stream state, flow control, and priority information.
///
/// Thread safety: NOT thread-safe. Streams are managed by the connection
/// on the single-threaded event loop.
class Http2Stream {
 public:
  /// Create a new stream with the given ID.
  /// Client-initiated streams have odd IDs, server-initiated have even IDs.
  explicit Http2Stream(uint32_t streamId, uint32_t initialWindowSize = kDefaultInitialWindowSize) noexcept;

  /// Get the stream identifier.
  [[nodiscard]] uint32_t id() const noexcept { return _streamId; }

  /// Get the current stream state.
  [[nodiscard]] StreamState state() const noexcept { return _state; }

  /// Check if the stream is in a state that can send frames.
  [[nodiscard]] bool canSend() const noexcept {
    return _state == StreamState::Open || _state == StreamState::HalfClosedRemote;
  }

  /// Check if the stream is in a state that can receive frames.
  [[nodiscard]] bool canReceive() const noexcept {
    return _state == StreamState::Open || _state == StreamState::HalfClosedLocal;
  }

  /// Check if the stream is closed.
  [[nodiscard]] bool isClosed() const noexcept { return _state == StreamState::Closed; }

  /// Mark the stream as "closed notified".
  ///
  /// This is used by the connection to ensure stream-close accounting (active stream count,
  /// callbacks, etc.) happens exactly once.
  /// @return True if this is the first time the stream is marked, false otherwise.
  [[nodiscard]] bool markClosedNotified() noexcept {
    if (_closedNotified) {
      return false;
    }
    _closedNotified = true;
    return true;
  }

  // ============================
  // State transitions
  // ============================

  /// Transition state when sending HEADERS frame.
  /// @param endStream True if END_STREAM flag is set
  /// @return ErrorCode if the transition is invalid, NoError otherwise
  [[nodiscard]] ErrorCode onSendHeaders(bool endStream) noexcept;

  /// Transition state when receiving HEADERS frame.
  /// @param endStream True if END_STREAM flag is set
  /// @return ErrorCode if the transition is invalid, NoError otherwise
  [[nodiscard]] ErrorCode onRecvHeaders(bool endStream) noexcept;

  /// Transition state when sending DATA frame.
  /// @param endStream True if END_STREAM flag is set
  /// @return ErrorCode if the transition is invalid, NoError otherwise
  [[nodiscard]] ErrorCode onSendData(bool endStream) noexcept;

  /// Transition state when receiving DATA frame.
  /// @param endStream True if END_STREAM flag is set
  /// @return ErrorCode if the transition is invalid, NoError otherwise
  [[nodiscard]] ErrorCode onRecvData(bool endStream) noexcept;

  /// Transition state when sending RST_STREAM.
  void onSendRstStream() noexcept { _state = StreamState::Closed; }

  /// Transition state when receiving RST_STREAM.
  void onRecvRstStream() noexcept { _state = StreamState::Closed; }

  /// Transition state when sending PUSH_PROMISE (server only).
  /// @return ErrorCode if the transition is invalid, NoError otherwise
  [[nodiscard]] ErrorCode onSendPushPromise() noexcept;

  /// Transition state when receiving PUSH_PROMISE (client only).
  /// @return ErrorCode if the transition is invalid, NoError otherwise
  [[nodiscard]] ErrorCode onRecvPushPromise() noexcept;

  // ============================
  // Flow control
  // ============================

  /// Get the current send window size.
  [[nodiscard]] int32_t sendWindow() const noexcept { return _sendWindow; }

  /// Get the current receive window size.
  [[nodiscard]] int32_t recvWindow() const noexcept { return _recvWindow; }

  /// Consume bytes from the send window (when sending DATA).
  /// @param bytes Number of bytes to consume
  /// @return True if there was sufficient window, false otherwise
  [[nodiscard]] bool consumeSendWindow(uint32_t bytes) noexcept;

  /// Consume bytes from the receive window (when receiving DATA).
  /// @param bytes Number of bytes to consume
  /// @return True if there was sufficient window, false otherwise
  [[nodiscard]] bool consumeRecvWindow(uint32_t bytes) noexcept;

  /// Increase the send window (from WINDOW_UPDATE).
  /// @param increment Window size increment
  /// @return ErrorCode if overflow would occur, NoError otherwise
  [[nodiscard]] ErrorCode increaseSendWindow(uint32_t increment) noexcept;

  /// Increase the receive window (when sending WINDOW_UPDATE).
  /// @param increment Window size increment
  /// @return ErrorCode if overflow would occur (FlowControlError), NoError otherwise
  [[nodiscard]] ErrorCode increaseRecvWindow(uint32_t increment) noexcept;

  /// Update the initial window size from SETTINGS.
  /// Adjusts the current send window by the delta.
  /// @param newInitialWindowSize New initial window size from SETTINGS
  /// @return ErrorCode if overflow would occur, NoError otherwise
  [[nodiscard]] ErrorCode updateInitialWindowSize(uint32_t newInitialWindowSize) noexcept;

  // ============================
  // Priority (RFC 9113 §5.3)
  // ============================

  /// Get the stream this stream depends on.
  [[nodiscard]] uint32_t streamDependency() const noexcept { return _streamDependency; }

  /// Get the weight of this stream (1-256).
  [[nodiscard]] uint16_t weight() const noexcept { return _weight; }

  /// Check if this stream has exclusive dependency.
  [[nodiscard]] bool isExclusive() const noexcept { return _exclusive; }

  /// Update priority information.
  void setPriority(uint32_t streamDependency, uint16_t weight, bool exclusive) noexcept {
    _streamDependency = streamDependency;
    _weight = weight;
    _exclusive = exclusive;
  }

  // ============================
  // Error handling
  // ============================

  /// Get the error code if the stream was reset.
  [[nodiscard]] ErrorCode errorCode() const noexcept { return _errorCode; }

  /// Set the error code when resetting the stream.
  void setErrorCode(ErrorCode code) noexcept { _errorCode = code; }

 private:
  uint32_t _streamId;
  ErrorCode _errorCode{ErrorCode::NoError};

  // Flow control
  int32_t _sendWindow;
  int32_t _recvWindow;
  uint32_t _initialSendWindow;

  // Priority
  uint32_t _streamDependency{0};
  uint16_t _weight{16};  // 1-256 (default is 16)
  StreamState _state{StreamState::Idle};
  bool _exclusive{false};
  bool _closedNotified{false};
};

}  // namespace aeronet::http2
