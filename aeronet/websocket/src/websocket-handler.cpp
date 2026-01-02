#include "aeronet/websocket-handler.hpp"

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/protocol-handler.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-deflate.hpp"
#include "aeronet/websocket-frame.hpp"

namespace aeronet::websocket {

WebSocketHandler::WebSocketHandler(WebSocketConfig config, WebSocketCallbacks callbacks,
                                   std::optional<DeflateNegotiatedParams> deflateParams)
    : _config(std::move(config)), _callbacks(std::move(callbacks)) {
  if (deflateParams.has_value()) {
    _deflateContext = std::make_unique<DeflateContext>(*deflateParams, _config.deflateConfig, _config.isServerSide);
  }
}

WebSocketHandler::~WebSocketHandler() = default;

WebSocketHandler::WebSocketHandler(WebSocketHandler&&) noexcept = default;
WebSocketHandler& WebSocketHandler::operator=(WebSocketHandler&&) noexcept = default;

void WebSocketHandler::setCallbacks(WebSocketCallbacks callbacks) { _callbacks = std::move(callbacks); }

ProtocolProcessResult WebSocketHandler::processInput(std::span<const std::byte> data, ConnectionState& /*state*/) {
  ProtocolProcessResult result;

  // Append new data to any carry-over from previous call
  if (!_inputBuffer.empty()) {
    _inputBuffer.append(data);
    data = _inputBuffer;
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
      } else {
        // Shift remaining data to front
        _inputBuffer.erase_front(totalConsumed);
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
    if (frameProcessResult.action == ProtocolProcessResult::Action::Close ||
        frameProcessResult.action == ProtocolProcessResult::Action::CloseImmediate) {
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
  result.bytesConsumed = totalConsumed;
  return result;
}

ProtocolProcessResult WebSocketHandler::processFrame(const FrameParseResult& frame) {
  // Unmask payload if needed (creates a copy)
  std::span<const std::byte> payload = frame.payload;
  RawBytes unmaskedPayload;

  if (frame.header.masked) {
    unmaskedPayload.append(payload);
    ApplyMask(unmaskedPayload, frame.header.maskingKey);
    payload = unmaskedPayload;
  }

  // Dispatch based on opcode type
  if (IsControlFrame(frame.header.opcode)) {
    return handleControlFrame(frame.header, payload);
  }
  return handleDataFrame(frame.header, payload);
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

    // Start new message
    _message.opcode = header.opcode;
    _message.inProgress = true;
    _message.buffer.clear();

    // Per RFC 7692: RSV1 is set only on the first frame of a compressed message
    _messageCompressed = header.rsv1;
  }

  // Check message size limit
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

  // Append payload to message buffer
  _message.buffer.append(payload);

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

    case Opcode::Close: {
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

    default:
      // Unknown control opcode - should not reach here via public API; throw for visibility
      throw std::logic_error("handleControlFrame: unexpected control opcode encountered");
  }

  return result;
}

namespace {

/// Validate UTF-8 encoding for text messages.
/// @return true if valid UTF-8, false otherwise
bool ValidateUtf8(std::span<const std::byte> data) {
  // UTF-8 validation state machine
  // See RFC 3629 for UTF-8 encoding rules
  const auto* ptr = reinterpret_cast<const uint8_t*>(data.data());
  const auto* end = ptr + data.size();

  while (ptr < end) {
    uint8_t byte = *ptr++;

    if (byte <= 0x7F) {
      // ASCII - single byte
      continue;
    }

    std::size_t remaining = 0;
    uint32_t codepoint = 0;
    uint32_t minCodepoint = 0;

    if ((byte & 0xE0) == 0xC0) {
      // 2-byte sequence
      remaining = 1;
      codepoint = byte & 0x1F;
      minCodepoint = 0x80;
    } else if ((byte & 0xF0) == 0xE0) {
      // 3-byte sequence
      remaining = 2;
      codepoint = byte & 0x0F;
      minCodepoint = 0x800;
    } else if ((byte & 0xF8) == 0xF0) {
      // 4-byte sequence
      remaining = 3;
      codepoint = byte & 0x07;
      minCodepoint = 0x10000;
    } else {
      // Invalid leading byte
      return false;
    }

    if (ptr + remaining > end) {
      // Incomplete sequence at end
      return false;
    }

    for (std::size_t idx = 0; idx < remaining; ++idx) {
      byte = *ptr++;
      if ((byte & 0xC0) != 0x80) {
        // Invalid continuation byte
        return false;
      }
      codepoint = (codepoint << 6) | (byte & 0x3F);
    }

    // Check for overlong encoding
    if (codepoint < minCodepoint) {
      return false;
    }

    // Check for surrogate pairs (invalid in UTF-8)
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
      return false;
    }

    // Check for out of range
    if (codepoint > 0x10FFFF) {
      return false;
    }
  }

  return true;
}

}  // namespace

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

bool WebSocketHandler::hasPendingOutput() const noexcept { return _outputOffset < _outputBuffer.size(); }

std::span<const std::byte> WebSocketHandler::getPendingOutput() {
  assert(_outputOffset <= _outputBuffer.size());
  return {_outputBuffer.begin() + _outputOffset, _outputBuffer.end()};
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
}

void WebSocketHandler::queueFrame(Opcode opcode, std::span<const std::byte> payload, bool fin) {
  // Server should NOT mask outgoing frames
  bool shouldMask = !_config.isServerSide;

  // For simplicity, we use a zero mask when not masking
  // In a real client implementation, we'd generate random masking keys
  MaskingKey mask{};

  BuildFrame(_outputBuffer, opcode, payload, fin, shouldMask, mask, false);
}

bool WebSocketHandler::sendText(std::string_view text) {
  if (_closeState != CloseState::Open) {
    return false;
  }

  auto textSpan = std::as_bytes(std::span(text.data(), text.size()));

  // Try to compress if compression is enabled and payload is large enough
  if (_deflateContext && !_deflateContext->shouldSkipCompression(text.size())) {
    _compressBuffer.clear();
    const char* errMsg = _deflateContext->compress(textSpan, _compressBuffer);
    if (errMsg == nullptr) {
      // Only use compressed data if it's smaller
      if (_compressBuffer.size() < text.size()) {
        auto compressedSpan = std::as_bytes(std::span(_compressBuffer.data(), _compressBuffer.size()));
        bool shouldMask = !_config.isServerSide;
        MaskingKey mask{};
        BuildFrame(_outputBuffer, Opcode::Text, compressedSpan, true, shouldMask, mask, true);  // RSV1=true
        return true;
      }
    }
  }

  queueFrame(Opcode::Text, textSpan);
  return true;
}

bool WebSocketHandler::sendBinary(std::span<const std::byte> data) {
  if (_closeState != CloseState::Open) {
    return false;
  }

  // Try to compress if compression is enabled and payload is large enough
  if (_deflateContext && !_deflateContext->shouldSkipCompression(data.size())) {
    _compressBuffer.clear();
    const char* errMsg = _deflateContext->compress(data, _compressBuffer);
    if (errMsg == nullptr) {
      // Only use compressed data if it's smaller
      if (_compressBuffer.size() < data.size()) {
        auto compressedSpan = std::as_bytes(std::span(_compressBuffer.data(), _compressBuffer.size()));
        bool shouldMask = !_config.isServerSide;
        MaskingKey mask{};
        BuildFrame(_outputBuffer, Opcode::Binary, compressedSpan, true, shouldMask, mask, true);  // RSV1=true
        return true;
      }
    }
  }

  queueFrame(Opcode::Binary, data);
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

  BuildCloseFrame(_outputBuffer, code, reason, !_config.isServerSide);

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
