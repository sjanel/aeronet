#include "aeronet/websocket-handler.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string_view>
#include <utility>

#ifdef __SSE2__
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

#include "aeronet/protocol-handler.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-deflate.hpp"
#include "aeronet/websocket-frame.hpp"

namespace aeronet::websocket {

namespace {

/// Validate a single multibyte UTF-8 sequence starting at *ptr.
/// @param ptr  Points to 1 byte past the lead byte on entry; advanced past the full sequence on success.
/// @param end  One-past-end of the buffer.
/// @param lead The leading byte of the sequence (already consumed).
/// @return true if the sequence is valid.
bool ValidateMultibyteSequence(const uint8_t*& ptr, const uint8_t* end, uint8_t lead) {
  std::size_t remaining;
  uint32_t codepoint;
  uint32_t minCodepoint;

  if ((lead & 0xE0) == 0xC0) {
    remaining = 1;
    codepoint = lead & 0x1F;
    minCodepoint = 0x80;
  } else if ((lead & 0xF0) == 0xE0) {
    remaining = 2;
    codepoint = lead & 0x0F;
    minCodepoint = 0x800;
  } else if ((lead & 0xF8) == 0xF0) {
    remaining = 3;
    codepoint = lead & 0x07;
    minCodepoint = 0x10000;
  } else {
    return false;
  }

  if (ptr + remaining > end) {
    return false;
  }

  for (std::size_t idx = 0; idx < remaining; ++idx) {
    uint8_t byte = *ptr++;
    if ((byte & 0xC0) != 0x80) {
      return false;
    }
    codepoint = (codepoint << 6) | (byte & 0x3F);
  }

  if (codepoint < minCodepoint) {
    return false;
  }
  if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
    return false;
  }
  return codepoint <= 0x10FFFF;
}

/// Validate UTF-8 encoding for text messages.
/// Uses SIMD to fast-skip all-ASCII chunks (16 bytes at a time).
/// @return true if valid UTF-8, false otherwise
bool ValidateUtf8(std::span<const std::byte> data) {
  if (data.empty()) {
    return true;
  }
  const auto* ptr = reinterpret_cast<const uint8_t*>(data.data());
  const auto* end = ptr + data.size();

#ifdef __SSE2__
  // Fast-scan 16 bytes at a time: if all bytes are ASCII (high bit=0), skip the whole chunk.
  while (ptr + 16 <= end) {
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));
    int mask = _mm_movemask_epi8(chunk);
    if (mask == 0) {
      // All 16 bytes are ASCII
      ptr += 16;
      continue;
    }
    // At least one non-ASCII byte — find it and validate the multibyte sequence
    auto offset = static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>(mask)));
    ptr += offset;
    uint8_t lead = *ptr++;
    if (!ValidateMultibyteSequence(ptr, end, lead)) {
      return false;
    }
  }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  while (ptr + 16 <= end) {
    uint8x16_t chunk = vld1q_u8(ptr);
    // Check if any byte has the high bit set (>= 0x80)
    if (vmaxvq_u8(chunk) < 0x80) {
      ptr += 16;
      continue;
    }
    // Find first non-ASCII byte via scalar scan within this 16-byte window
    while (ptr < end && *ptr <= 0x7F) {
      ++ptr;
    }
    if (ptr >= end) {
      break;
    }
    uint8_t lead = *ptr++;
    if (!ValidateMultibyteSequence(ptr, end, lead)) {
      return false;
    }
  }
#endif

  // Scalar tail (< 16 bytes remaining, or no SIMD)
  while (ptr < end) {
    uint8_t byte = *ptr++;
    if (byte <= 0x7F) {
      continue;
    }
    if (!ValidateMultibyteSequence(ptr, end, byte)) {
      return false;
    }
  }

  return true;
}

}  // namespace

WebSocketHandler::WebSocketHandler(WebSocketConfig config, WebSocketCallbacks callbacks,
                                   std::optional<DeflateNegotiatedParams> deflateParams)
    : _config(std::move(config)), _callbacks(std::move(callbacks)) {
  _config.validate();
  if (deflateParams.has_value()) {
    _deflateContext = std::make_unique<DeflateContext>(*deflateParams, _config.deflateConfig, _config.isServerSide);
  }
}

WebSocketHandler::~WebSocketHandler() = default;

WebSocketHandler::WebSocketHandler(WebSocketHandler&&) noexcept = default;
WebSocketHandler& WebSocketHandler::operator=(WebSocketHandler&&) noexcept = default;

void WebSocketHandler::setCallbacks(WebSocketCallbacks callbacks) { _callbacks = std::move(callbacks); }

ProtocolProcessResult WebSocketHandler::processInput(std::span<const std::byte> data,
                                                     [[maybe_unused]] ConnectionState& state) {
  ProtocolProcessResult result;

  // Append new data to any carry-over from previous call
  if (_inputBufferOffset < _inputBuffer.size()) {
    _inputBuffer.append(data);
    data = {_inputBuffer.begin() + _inputBufferOffset, _inputBuffer.end()};
  }

  std::size_t totalConsumed = 0;
  const bool allowRsv1 = (_deflateContext != nullptr);

  // Process as many complete frames as possible
  while (!data.empty()) {
    auto frameResult = ParseFrame(data, _config.maxFrameSize, _config.isServerSide, allowRsv1);

    if (frameResult.status == FrameParseResult::Status::Incomplete) {
      // Need more data - save remainder for next call
      if (_inputBuffer.empty()) {
        _inputBuffer.append(data);
        _inputBufferOffset = 0;
      } else {
        // Advance offset instead of memmove
        _inputBufferOffset += totalConsumed;
        // Compact if offset exceeds half the buffer to avoid unbounded growth
        if (_inputBufferOffset > _inputBuffer.size() / 2) {
          _inputBuffer.erase_front(_inputBufferOffset);
          _inputBufferOffset = 0;
        }
      }
      result.bytesConsumed = totalConsumed;
      return result;
    }

    if (frameResult.status == FrameParseResult::Status::ProtocolError) {
      // Protocol violation - close with error
      if (_callbacks.onError) {
        _callbacks.onError(CloseCode::ProtocolError, frameResult.errorMessage);
      }
      sendClose(CloseCode::ProtocolError, frameResult.errorMessage);
      result.action = ProtocolProcessResult::Action::Close;
      result.bytesConsumed = totalConsumed;
      return result;
    }

    if (frameResult.status == FrameParseResult::Status::PayloadTooLarge) {
      if (_callbacks.onError) {
        _callbacks.onError(CloseCode::MessageTooBig, "Frame payload too large");
      }
      sendClose(CloseCode::MessageTooBig, "Frame payload too large");
      result.action = ProtocolProcessResult::Action::Close;
      result.bytesConsumed = totalConsumed;
      return result;
    }

    // Frame complete - process it
    auto frameProcessResult = processFrame(frameResult);

    totalConsumed += frameResult.bytesConsumed;
    data = data.subspan(frameResult.bytesConsumed);

    // Check if we need to stop processing
    // processFrame() never returns CloseImmediate in the WebSocket protocol (only HTTP/2 does)
    assert(frameProcessResult.action != ProtocolProcessResult::Action::CloseImmediate);
    if (frameProcessResult.action == ProtocolProcessResult::Action::Close) {
      result.action = frameProcessResult.action;
      result.bytesConsumed = totalConsumed;
      _inputBuffer.clear();
      return result;
    }

    if (frameProcessResult.action == ProtocolProcessResult::Action::ResponseReady) {
      result.action = ProtocolProcessResult::Action::ResponseReady;
    }
  }

  // All data consumed
  _inputBuffer.clear();
  _inputBufferOffset = 0;
  result.bytesConsumed = totalConsumed;
  return result;
}

ProtocolProcessResult WebSocketHandler::processFrame(const FrameParseResult& frame) {
  if (IsControlFrame(frame.header.opcode)) {
    // Control frames max 125 bytes - unmask on stack
    std::span<const std::byte> payload = frame.payload;
    std::array<std::byte, kMaxControlFramePayload> controlBuf;
    if (frame.header.masked && !payload.empty()) {
      std::memcpy(controlBuf.data(), payload.data(), payload.size());
      ApplyMask(std::span<std::byte>(controlBuf.data(), payload.size()), frame.header.maskingKey);
      payload = std::span<const std::byte>(controlBuf.data(), payload.size());
    }
    return handleControlFrame(frame.header, payload);
  }
  // Data frames: pass raw payload, unmasking happens in-place in message buffer
  return handleDataFrame(frame.header, frame.payload);
}

ProtocolProcessResult WebSocketHandler::handleDataFrame(const FrameHeader& header, std::span<const std::byte> payload) {
  ProtocolProcessResult result;

  if (header.opcode == Opcode::Continuation) {
    // Continuation frame - must be in a fragmented message
    if (!_message.inProgress) {
      if (_callbacks.onError) {
        _callbacks.onError(CloseCode::ProtocolError, "Unexpected continuation frame");
      }
      sendClose(CloseCode::ProtocolError, "Unexpected continuation frame");
      result.action = ProtocolProcessResult::Action::Close;
      return result;
    }
  } else {
    // Text or Binary - must NOT be in a fragmented message
    if (_message.inProgress) {
      if (_callbacks.onError) {
        _callbacks.onError(CloseCode::ProtocolError, "Expected continuation frame");
      }
      sendClose(CloseCode::ProtocolError, "Expected continuation frame");
      result.action = ProtocolProcessResult::Action::Close;
      return result;
    }

    // Fast path: single complete non-fragmented non-compressed frame.
    // Bypass the message reassembly buffer entirely — unmask the payload in-place
    // and deliver directly to the callback without any extra allocation or copy.
    // Safety: 'payload' is a view into the caller's TCP receive buffer (pState->inBuffer),
    // which is mutable memory cast to const at the call site. Modifying it here is safe
    // because the buffer will be consumed (erased) immediately after processInput returns.
    if (header.fin && _deflateContext == nullptr) {
      if (_config.maxMessageSize > 0 && payload.size() > _config.maxMessageSize) {
        if (_callbacks.onError) {
          _callbacks.onError(CloseCode::MessageTooBig, "Message too large");
        }
        sendClose(CloseCode::MessageTooBig, "Message too large");
        result.action = ProtocolProcessResult::Action::Close;
        return result;
      }
      if (header.masked && !payload.empty()) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        ApplyMask(std::span<std::byte>(const_cast<std::byte*>(payload.data()), payload.size()), header.maskingKey);
      }

      /// Deliver a single complete non-fragmented non-compressed frame directly to the callback
      /// without copying the payload through the message buffer (zero-copy fast path).
      /// The payload must already be unmasked when this is called.
      if (header.opcode == Opcode::Text) {
        if (!ValidateUtf8(payload)) {
          if (_callbacks.onError) {
            _callbacks.onError(CloseCode::InvalidPayloadData, "Invalid UTF-8 in text message");
          }
          sendClose(CloseCode::InvalidPayloadData, "Invalid UTF-8 in text message");
          result.action = ProtocolProcessResult::Action::Close;
          return result;
        }
      }

      if (_callbacks.onMessage) {
        _callbacks.onMessage(payload, header.opcode == Opcode::Binary);
      }

      result.action = ProtocolProcessResult::Action::Continue;
      return result;
    }

    // Fragmented or compressed message: start accumulating in message buffer
    _message.opcode = header.opcode;
    _message.inProgress = true;
    _message.buffer.clear();

    // Per RFC 7692: RSV1 is set only on the first frame of a compressed message
    _messageCompressed = header.rsv1;
  }

  // Check message size limit (fragmented / compressed path)
  std::size_t newSize = _message.buffer.size() + payload.size();
  if (_config.maxMessageSize > 0 && newSize > _config.maxMessageSize) {
    if (_callbacks.onError) {
      _callbacks.onError(CloseCode::MessageTooBig, "Message too large");
    }
    sendClose(CloseCode::MessageTooBig, "Message too large");
    result.action = ProtocolProcessResult::Action::Close;
    _message.inProgress = false;
    _message.buffer.clear();
    return result;
  }

  // Append payload to message buffer and unmask in-place (avoids extra copy)
  const auto prevSize = _message.buffer.size();
  _message.buffer.append(payload);
  if (header.masked && !payload.empty()) {
    ApplyMask(std::span<std::byte>(_message.buffer.data() + prevSize, payload.size()), header.maskingKey);
  }

  // If FIN bit is set, message is complete
  if (header.fin) {
    return completeMessage();
  }

  // More fragments expected
  result.action = ProtocolProcessResult::Action::Continue;
  return result;
}

ProtocolProcessResult WebSocketHandler::handleControlFrame(const FrameHeader& header,
                                                           std::span<const std::byte> payload) {
  ProtocolProcessResult result;

  switch (header.opcode) {
    case Opcode::Ping:
      // Respond with Pong containing same payload
      sendPong(payload);
      result.action = ProtocolProcessResult::Action::ResponseReady;

      if (_callbacks.onPing) {
        _callbacks.onPing(payload);
      }
      break;

    case Opcode::Pong:
      // Informational only
      if (_callbacks.onPong) {
        _callbacks.onPong(payload);
      }
      result.action = ProtocolProcessResult::Action::Continue;
      break;

    default: {
      // Opcode::Close
      assert(header.opcode == Opcode::Close);
      const auto closeInfo = ParseClosePayload(payload);

      if (_closeState == CloseState::Open) {
        // Peer initiated close - respond with Close
        _closeState = CloseState::CloseReceived;
        _closeCode = closeInfo.code;
        sendClose(closeInfo.code, closeInfo.reason);
        _closeState = CloseState::Closed;
        result.action = ProtocolProcessResult::Action::ResponseReady;
      } else if (_closeState == CloseState::CloseSent) {
        // We initiated, peer responded - handshake complete
        _closeState = CloseState::Closed;
        result.action = ProtocolProcessResult::Action::Close;
      }

      if (_callbacks.onClose) {
        _callbacks.onClose(closeInfo.code, closeInfo.reason);
      }
      break;
    }
  }

  return result;
}

ProtocolProcessResult WebSocketHandler::completeMessage() {
  ProtocolProcessResult result;

  // Decompress if message was compressed
  std::span<const std::byte> messageData;
  if (_messageCompressed && _deflateContext) {
    _compressBuffer.clear();
    const char* errMsg = _deflateContext->decompress(_message.buffer, _compressBuffer, _config.maxMessageSize);
    if (errMsg != nullptr) {
      if (_callbacks.onError) {
        _callbacks.onError(CloseCode::InvalidPayloadData, "Decompression failed");
      }
      sendClose(CloseCode::InvalidPayloadData, "Decompression failed");
      result.action = ProtocolProcessResult::Action::Close;
      _message.inProgress = false;
      _message.buffer.clear();
      _messageCompressed = false;
      return result;
    }
    messageData = _compressBuffer;
  } else {
    messageData = _message.buffer;
  }

  // For text messages, validate UTF-8
  if (_message.opcode == Opcode::Text) {
    if (!ValidateUtf8(messageData)) {
      if (_callbacks.onError) {
        _callbacks.onError(CloseCode::InvalidPayloadData, "Invalid UTF-8 in text message");
      }
      sendClose(CloseCode::InvalidPayloadData, "Invalid UTF-8 in text message");
      result.action = ProtocolProcessResult::Action::Close;
      _message.inProgress = false;
      _message.buffer.clear();
      _messageCompressed = false;
      return result;
    }
  }

  // Invoke callback
  if (_callbacks.onMessage) {
    _callbacks.onMessage(messageData, _message.opcode == Opcode::Binary);
  }

  // Reset message state
  _message.inProgress = false;
  _message.buffer.clear();
  _messageCompressed = false;

  result.action = ProtocolProcessResult::Action::Continue;
  return result;
}

void WebSocketHandler::onOutputWritten(std::size_t bytesWritten) {
  _outputOffset += bytesWritten;

  // If all output has been written, clear the buffer
  if (_outputOffset >= _outputBuffer.size()) {
    _outputBuffer.clear();
    _outputOffset = 0;
  }
}

void WebSocketHandler::initiateClose() {
  if (_closeState == CloseState::Open) {
    sendClose(CloseCode::GoingAway, "Server shutting down");
  }
}

void WebSocketHandler::onTransportClosing() {
  _closeState = CloseState::Closed;
  _message.inProgress = false;
  _message.buffer.clear();
  _inputBuffer.clear();
  _inputBufferOffset = 0;
}

namespace {

/// Generate a random masking key for client-side frames (RFC 6455 §10.3).
MaskingKey GenerateRandomMaskingKey() {
  thread_local std::mt19937 rng(std::random_device{}());
  return static_cast<MaskingKey>(rng());
}

}  // namespace

void WebSocketHandler::queueFrame(Opcode opcode, std::span<const std::byte> payload, bool fin) {
  const bool shouldMask = !_config.isServerSide;
  const MaskingKey mask = shouldMask ? GenerateRandomMaskingKey() : MaskingKey{};
  BuildFrame(_outputBuffer, opcode, payload, fin, shouldMask, mask, false);
}

bool WebSocketHandler::sendData(Opcode opcode, std::span<const std::byte> payload) {
  if (_closeState != CloseState::Open) {
    return false;
  }

  // Try to compress if compression is enabled and payload is large enough
  if (_deflateContext && !_deflateContext->shouldSkipCompression(payload.size())) {
    _compressBuffer.clear();
    const char* errMsg = _deflateContext->compress(payload, _compressBuffer);
    if (errMsg == nullptr && _compressBuffer.size() < payload.size()) {
      const bool shouldMask = !_config.isServerSide;
      const MaskingKey mask = shouldMask ? GenerateRandomMaskingKey() : MaskingKey{};
      BuildFrame(_outputBuffer, opcode, std::span<const std::byte>(_compressBuffer.data(), _compressBuffer.size()),
                 true, shouldMask, mask, true);
      return true;
    }
  }

  queueFrame(opcode, payload);
  return true;
}

bool WebSocketHandler::sendPing(std::span<const std::byte> payload) {
  if (_closeState != CloseState::Open) {
    return false;
  }

  // Control frames have max 125 bytes payload
  if (payload.size() > kMaxControlFramePayload) {
    payload = payload.first(kMaxControlFramePayload);
  }

  queueFrame(Opcode::Ping, payload);
  return true;
}

bool WebSocketHandler::sendPong(std::span<const std::byte> payload) {
  // Pong can be sent even during close handshake (per RFC 6455)
  if (_closeState == CloseState::Closed) {
    return false;
  }

  if (payload.size() > kMaxControlFramePayload) {
    payload = payload.first(kMaxControlFramePayload);
  }

  queueFrame(Opcode::Pong, payload);
  return true;
}

bool WebSocketHandler::sendClose(CloseCode code, std::string_view reason) {
  if (_closeState == CloseState::CloseSent || _closeState == CloseState::Closed) {
    return false;
  }

  const bool shouldMask = !_config.isServerSide;
  const MaskingKey mask = shouldMask ? GenerateRandomMaskingKey() : MaskingKey{};
  BuildCloseFrame(_outputBuffer, code, reason, shouldMask, mask);

  if (_closeState == CloseState::Open) {
    _closeState = CloseState::CloseSent;
    _closeInitiatedAt = std::chrono::steady_clock::now();
  }
  _closeCode = code;

  return true;
}

std::unique_ptr<WebSocketHandler> CreateServerWebSocketHandler(WebSocketCallbacks callbacks,
                                                               std::size_t maxMessageSize) {
  WebSocketConfig config;
  config.isServerSide = true;
  config.maxMessageSize = maxMessageSize;
  return std::make_unique<WebSocketHandler>(config, std::move(callbacks));
}

std::unique_ptr<WebSocketHandler> CreateClientWebSocketHandler(WebSocketCallbacks callbacks,
                                                               std::size_t maxMessageSize) {
  WebSocketConfig config;
  config.isServerSide = false;
  config.maxMessageSize = maxMessageSize;
  return std::make_unique<WebSocketHandler>(config, std::move(callbacks));
}

}  // namespace aeronet::websocket
