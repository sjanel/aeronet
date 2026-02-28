#include "aeronet/http2-stream.hpp"

#include <cstdint>
#include <limits>
#include <utility>

#include "aeronet/http2-frame-types.hpp"

namespace aeronet::http2 {

Http2Stream::Http2Stream(uint32_t streamId, uint32_t initialWindowSize) noexcept
    : _streamId(streamId),
      _sendWindow(static_cast<int32_t>(initialWindowSize)),
      _recvWindow(static_cast<int32_t>(initialWindowSize)),
      _initialSendWindow(initialWindowSize) {}

// ============================
// State transitions
// ============================

ErrorCode Http2Stream::onSendHeaders(bool endStream) noexcept {
  switch (_state) {
    case StreamState::Idle:
      _state = endStream ? StreamState::HalfClosedLocal : StreamState::Open;
      return ErrorCode::NoError;

    case StreamState::ReservedLocal:
      _state = endStream ? StreamState::Closed : StreamState::HalfClosedRemote;
      return ErrorCode::NoError;

    case StreamState::Open:
      // Sending headers or trailers
      if (endStream) {
        _state = StreamState::HalfClosedLocal;
      }
      return ErrorCode::NoError;

    case StreamState::HalfClosedRemote:
      // Remote is done sending, we can still send response headers/data
      if (endStream) {
        _state = StreamState::Closed;
      }
      return ErrorCode::NoError;

    default:
      return ErrorCode::StreamClosed;
  }
}

ErrorCode Http2Stream::onRecvHeaders(bool endStream) noexcept {
  switch (_state) {
    case StreamState::Idle:
      _state = endStream ? StreamState::HalfClosedRemote : StreamState::Open;
      return ErrorCode::NoError;

    case StreamState::ReservedRemote:
      _state = endStream ? StreamState::Closed : StreamState::HalfClosedLocal;
      return ErrorCode::NoError;

    case StreamState::Open:
      // Receiving response headers or trailers
      // If trailers, END_STREAM must be set
      if (endStream) {
        _state = StreamState::HalfClosedRemote;
      }
      return ErrorCode::NoError;

    case StreamState::HalfClosedLocal:
      // Client has finished sending request (END_STREAM sent)
      // Server can send response headers (with or without END_STREAM), then data, then trailers
      if (endStream) {
        _state = StreamState::Closed;
      }
      return ErrorCode::NoError;

    default:
      return ErrorCode::StreamClosed;
  }
}

ErrorCode Http2Stream::onSendData(bool endStream) noexcept {
  switch (_state) {
    case StreamState::Open:
      if (endStream) {
        _state = StreamState::HalfClosedLocal;
      }
      return ErrorCode::NoError;

    case StreamState::HalfClosedRemote:
      if (endStream) {
        _state = StreamState::Closed;
      }
      return ErrorCode::NoError;

    default:
      return ErrorCode::StreamClosed;
  }
}

ErrorCode Http2Stream::onRecvData(bool endStream) noexcept {
  switch (_state) {
    case StreamState::Open:
      if (endStream) {
        _state = StreamState::HalfClosedRemote;
      }
      return ErrorCode::NoError;

    case StreamState::HalfClosedLocal:
      if (endStream) {
        _state = StreamState::Closed;
      }
      return ErrorCode::NoError;

    default:
      return ErrorCode::StreamClosed;
  }
}

ErrorCode Http2Stream::onSendPushPromise() noexcept {
  // Server sends PUSH_PROMISE on a client-initiated stream
  // This reserves a new server-initiated stream (even ID)
  if (_state == StreamState::Open || _state == StreamState::HalfClosedRemote) {
    return ErrorCode::NoError;  // Parent stream state unchanged
  }
  return ErrorCode::ProtocolError;
}

ErrorCode Http2Stream::onRecvPushPromise() noexcept {
  // Client receives PUSH_PROMISE on a client-initiated stream
  // This reserves a new server-initiated stream (even ID)
  if (_state == StreamState::Open || _state == StreamState::HalfClosedLocal) {
    return ErrorCode::NoError;  // Parent stream state unchanged
  }
  return ErrorCode::ProtocolError;
}

// ============================
// Flow control
// ============================

bool Http2Stream::consumeSendWindow(uint32_t bytes) noexcept {
  if (std::cmp_less(_sendWindow, bytes)) {
    return false;
  }
  _sendWindow -= static_cast<int32_t>(bytes);
  return true;
}

bool Http2Stream::consumeRecvWindow(uint32_t bytes) noexcept {
  if (std::cmp_less(_recvWindow, bytes)) {
    return false;
  }
  _recvWindow -= static_cast<int32_t>(bytes);
  return true;
}

ErrorCode Http2Stream::increaseSendWindow(uint32_t increment) noexcept {
  // Check for overflow: RFC 9113 §6.9 specifies max window size as 2^31-1
  static constexpr int32_t kMaxWindowSize = std::numeric_limits<int32_t>::max();

  if (increment == 0) {
    return ErrorCode::ProtocolError;
  }

  int64_t newWindow = static_cast<int64_t>(_sendWindow) + static_cast<int64_t>(increment);
  if (newWindow > kMaxWindowSize) {
    return ErrorCode::FlowControlError;
  }

  _sendWindow = static_cast<int32_t>(newWindow);
  return ErrorCode::NoError;
}

ErrorCode Http2Stream::increaseRecvWindow(uint32_t increment) noexcept {
  // Security hardening: check for overflow. RFC 9113 §6.9.1 mandates that a window
  // size must never exceed 2^31-1 (kMaxWindowSize). Without this check, a bug or
  // double-increment could silently overflow the int32_t window.
  static constexpr int32_t kMaxWindowSize = std::numeric_limits<int32_t>::max();

  int64_t newWindow = static_cast<int64_t>(_recvWindow) + static_cast<int64_t>(increment);
  if (newWindow > kMaxWindowSize) [[unlikely]] {
    return ErrorCode::FlowControlError;
  }

  _recvWindow = static_cast<int32_t>(newWindow);
  return ErrorCode::NoError;
}

ErrorCode Http2Stream::updateInitialWindowSize(uint32_t newInitialWindowSize) noexcept {
  // RFC 9113 §6.9.2: When the value of SETTINGS_INITIAL_WINDOW_SIZE changes,
  // a receiver MUST adjust the size of all stream flow-control windows that
  // it maintains by the difference between the new value and the old value.

  // Compute delta in 64-bit to avoid overflow
  int64_t delta = static_cast<int64_t>(newInitialWindowSize) - static_cast<int64_t>(_initialSendWindow);

  // Check for overflow
  static constexpr int32_t kMaxWindowSize = std::numeric_limits<int32_t>::max();
  int64_t newWindow = static_cast<int64_t>(_sendWindow) + delta;

  if (newWindow > kMaxWindowSize || newWindow < 0) [[unlikely]] {
    return ErrorCode::FlowControlError;
  }

  _sendWindow = static_cast<int32_t>(newWindow);
  _initialSendWindow = newInitialWindowSize;
  return ErrorCode::NoError;
}

}  // namespace aeronet::http2
