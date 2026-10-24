#include "aeronet/websocket-handler.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/http-message-data.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-deflate.hpp"
#include "aeronet/websocket-frame.hpp"
#include "aeronet/websocket-utf8.hpp"

namespace aeronet::websocket {

namespace {

// Smallest payload sent from where it is (the received input, the compression buffer) instead of being copied to the
// output buffer: below, the copy is cheaper than the separate buffer to write.
constexpr std::size_t kMinUnbufferedPayloadSize = 4096;

// Tells whether 'part' lies within 'whole' (std::less_equal is a total order, even for unrelated pointers).
bool IsWithin(std::span<const std::byte> part, std::span<const std::byte> whole) {
  constexpr std::less_equal<> kLessEqual;
  return kLessEqual(whole.data(), part.data()) && kLessEqual(part.data() + part.size(), whole.data() + whole.size());
}

}  // namespace

WebSocketHandler::WebSocketHandler(WebSocketConfig config, WebSocketCallbacks callbacks,
                                   DeflateNegotiatedParams deflateParams)
    : _config(std::move(config)), _callbacks(std::move(callbacks)) {
  _config.validate();
  if (deflateParams.hasValue()) {
    enableCompression(deflateParams);
  }
  if (!_config.isServerSide) {
    // Seed splitmix64 from time + object address: no syscall, adequate per-connection uniqueness.
    // Masking is not a security mechanism (RFC 6455 §10.3 — proxy cache poisoning prevention only).
    _rngState = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()) ^
                reinterpret_cast<uintptr_t>(this);
  }
}

WebSocketHandler::~WebSocketHandler() = default;

WebSocketHandler::WebSocketHandler(WebSocketHandler&&) noexcept = default;
WebSocketHandler& WebSocketHandler::operator=(WebSocketHandler&&) noexcept = default;

void WebSocketHandler::setCallbacks(WebSocketCallbacks callbacks) { _callbacks = std::move(callbacks); }

void WebSocketHandler::enableCompression(DeflateNegotiatedParams deflateParams, SharedDeflateStreams* sharedStreams) {
  _deflateContext =
      std::make_unique<DeflateContext>(deflateParams, _config.deflateConfig, _config.isServerSide, sharedStreams);
}

ProtocolProcessResult WebSocketHandler::processInput(std::span<const std::byte> data) {
  // Only complete frames are consumed. The bytes of a trailing incomplete frame are left to the caller, which passes
  // them again, followed by the next received bytes, at the next call (they are not copied here).
  ProtocolProcessResult result;
  const bool allowRsv1 = (_deflateContext != nullptr);

  // While the frames are processed, sendData() sends the large payloads found in the input from there.
  struct InputScope {
    explicit InputScope(WebSocketHandler& handler, std::span<const std::byte> input) : _handler(handler) {
      _handler._input = input;
    }

    InputScope(const InputScope&) = delete;
    InputScope(InputScope&&) noexcept = delete;
    InputScope& operator=(const InputScope&) = delete;
    InputScope& operator=(InputScope&&) noexcept = delete;

    ~InputScope() { _handler._input = {}; }

    WebSocketHandler& _handler;
  } inputScope(*this, data);

  while (!data.empty()) {
    const auto frameResult = ParseFrame(data, _config.maxFrameSize, _config.isServerSide, allowRsv1);

    if (frameResult.status == FrameParseResult::Status::Incomplete) {
      break;
    }

    if (frameResult.status == FrameParseResult::Status::ProtocolError) {
      // Protocol violation - close with error
      if (_callbacks.onError) {
        _callbacks.onError(CloseCode::ProtocolError, frameResult.pErrorMessage);
      }
      sendClose(CloseCode::ProtocolError, frameResult.pErrorMessage);
      result.action = ProtocolProcessResult::Action::Close;
      break;
    }

    if (frameResult.status == FrameParseResult::Status::PayloadTooLarge) {
      if (_callbacks.onError) {
        _callbacks.onError(CloseCode::MessageTooBig, "Frame payload too large");
      }
      sendClose(CloseCode::MessageTooBig, "Frame payload too large");
      result.action = ProtocolProcessResult::Action::Close;
      break;
    }

    // Frame complete - process it
    ProtocolProcessResult frameProcessResult = processFrame(frameResult);

    result.bytesConsumed += frameResult.bytesConsumed;
    data = data.subspan(frameResult.bytesConsumed);

    // Check if we need to stop processing
    // processFrame() never returns CloseImmediate in the WebSocket protocol (only HTTP/2 does)
    assert(frameProcessResult.action != ProtocolProcessResult::Action::CloseImmediate);
    if (frameProcessResult.action == ProtocolProcessResult::Action::Close) {
      result.action = frameProcessResult.action;
      break;
    }

    if (frameProcessResult.action == ProtocolProcessResult::Action::ResponseReady) {
      result.action = ProtocolProcessResult::Action::ResponseReady;
    }
  }

  result.outputRefersToInput = unbufferedPayloadInInput();
  return result;
}

ProtocolProcessResult WebSocketHandler::processFrame(const FrameParseResult& frame) {
  if (IsControlFrame(frame.header.opcode)) {
    // Control frames max 125 bytes - unmasked in the same pass as copied on the stack
    std::span<const std::byte> payload = frame.payload;
    // Declared here because its memory may be pointed by payload
    std::byte controlBuf[kMaxControlFramePayload];
    if (frame.header.masked && !payload.empty()) {
      payload = {controlBuf, AppendMasked(payload, controlBuf, frame.header.maskingKey)};
    }
    return handleControlFrame(frame.header, payload);
  }
  // Data frames: pass raw payload, unmasked in place (single frame) or when copied to the message buffer (fragments)
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
    // With permessage-deflate, RSV1 tells whether the message is compressed (only allowed when negotiated).
    if (header.fin && !header.rsv1) {
      if (_config.maxMessageSize > 0 && payload.size() > _config.maxMessageSize) {
        if (_callbacks.onError) {
          _callbacks.onError(CloseCode::MessageTooBig, "Message too large");
        }
        sendClose(CloseCode::MessageTooBig, "Message too large");
        result.action = ProtocolProcessResult::Action::Close;
        return result;
      }
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      const std::span<std::byte> mutablePayload(const_cast<std::byte*>(payload.data()), payload.size());
      if (header.opcode == Opcode::Text) {
        // A masked text message is unmasked in the same pass as its UTF-8 is validated.
        const bool isValidUtf8 =
            header.masked ? UnmaskAndValidateUtf8(mutablePayload, header.maskingKey) : IsValidUtf8(payload);
        if (!isValidUtf8) {
          if (_callbacks.onError) {
            _callbacks.onError(CloseCode::InvalidPayloadData, "Invalid UTF-8 in text message");
          }
          sendClose(CloseCode::InvalidPayloadData, "Invalid UTF-8 in text message");
          result.action = ProtocolProcessResult::Action::Close;
          return result;
        }
      } else if (header.masked) {
        ApplyMask(mutablePayload, header.maskingKey);
      }

      // Deliver the payload directly to the callback, without copying it through the message buffer.
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
  const std::size_t newSize = _message.buffer.size() + payload.size();
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

  // Append payload to message buffer, unmasked in the same pass as copied
  if (header.masked) {
    _message.buffer.ensureAvailableCapacityExponential(payload.size());
    _message.buffer.setEnd(AppendMasked(payload, _message.buffer.data() + _message.buffer.size(), header.maskingKey));
  } else {
    _message.buffer.append(payload);
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
        // Peer initiated close - respond with Close. Both Close frames are then exchanged: a server closes the TCP
        // connection right after sending its response, so that it holds the TIME_WAIT state and not the client
        // (RFC 6455 section 7.1.1).
        _closeState = CloseState::CloseReceived;
        _closeCode = closeInfo.code;
        sendClose(closeInfo.code, closeInfo.reason);
        _closeState = CloseState::Closed;
        result.action =
            _config.isServerSide ? ProtocolProcessResult::Action::Close : ProtocolProcessResult::Action::ResponseReady;
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
  if (_messageCompressed && _deflateContext) {
    // The compressed payload of a message sent earlier may still wait in the compression buffer, written to below.
    if (!unbufferedPayloadInInput()) {
      appendUnbufferedPayload();
    }
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
    // The message buffer holds the decompressed message: the compression buffer must stay free for the messages that
    // onMessage sends (an echo of this one would otherwise be compressed into its own memory).
    _message.buffer.swap(_compressBuffer);
  }
  const std::span<const std::byte> messageData = _message.buffer;

  // For text messages, validate UTF-8
  if (_message.opcode == Opcode::Text && !IsValidUtf8(messageData)) {
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
  const std::size_t bufferedSize = _outputBuffer.size() - _outputOffset;
  if (bytesWritten < bufferedSize) {
    _outputOffset += bytesWritten;
    return;
  }
  // All the buffered output has been written, then the start of the payload sent from where it is.
  assert(bytesWritten - bufferedSize <= _unbufferedPayload.size());
  _outputBuffer.clear();
  _outputOffset = 0;
  _unbufferedPayload = _unbufferedPayload.subspan(bytesWritten - bufferedSize);
}

void WebSocketHandler::getPendingOutputFragments(vector<std::string_view>& fragments) const {
  fragments.clear();
  if (_outputOffset != _outputBuffer.size()) {
    fragments.emplace_back(reinterpret_cast<const char*>(_outputBuffer.data() + _outputOffset),
                           _outputBuffer.size() - _outputOffset);
  }
  if (!_unbufferedPayload.empty()) {
    fragments.emplace_back(reinterpret_cast<const char*>(_unbufferedPayload.data()), _unbufferedPayload.size());
  }
}

void WebSocketHandler::appendUnbufferedPayload() {
  if (!_unbufferedPayload.empty()) {
    _outputBuffer.append(_unbufferedPayload);
    _unbufferedPayload = {};
  }
}

bool WebSocketHandler::unbufferedPayloadInInput() const noexcept {
  return !_unbufferedPayload.empty() && !IsWithin(_unbufferedPayload, _compressBuffer);
}

void WebSocketHandler::releaseInput() {
  // A payload in the compression buffer stays there: only the next write to this buffer needs to copy it.
  if (unbufferedPayloadInInput()) {
    appendUnbufferedPayload();
  }
}

void WebSocketHandler::onTransportClosing() {
  _closeState = CloseState::Closed;
  _unbufferedPayload = {};
  _message.inProgress = false;
  _message.buffer.clear();
}

bool WebSocketHandler::drainOutputBuffer(HttpMessageData& dest) {
  appendUnbufferedPayload();
  if (_outputOffset >= _outputBuffer.size()) {
    return false;
  }
  // Fast path: move the buffer allocation directly into dest to avoid memcpy.
  // Conditions: no partial drain (_outputOffset == 0) and dest is empty.
  if (_outputOffset == 0 && dest.empty()) {
    dest = HttpMessageData(RawChars(std::move(_outputBuffer)));
    _outputBuffer = {};
    return true;
  }
  // Fall back to copy path (partial drain or non-empty dest).
  auto pending = getPendingOutput();
  dest.append(std::string_view(reinterpret_cast<const char*>(pending.data()), pending.size()));
  onOutputWritten(pending.size());
  return true;
}

namespace {

/// Generate a random masking key (splitmix64, client-side only).
constexpr MaskingKey GenerateMaskingKey(uint64_t& rngState) noexcept {
  // splitmix64: 8-byte state, adequate for non-cryptographic masking keys (RFC 6455 §10.3)
  rngState += 0x9e3779b97f4a7c15ULL;
  uint64_t val = rngState;
  val = (val ^ (val >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  val = (val ^ (val >> 27U)) * 0x94d049bb133111ebULL;
  val ^= (val >> 31U);
  return static_cast<MaskingKey>(val);
}

}  // namespace

void WebSocketHandler::queueFrame(Opcode opcode, std::span<const std::byte> payload, bool fin) {
  appendUnbufferedPayload();
  const bool shouldMask = !_config.isServerSide;
  const MaskingKey mask = shouldMask ? GenerateMaskingKey(_rngState) : MaskingKey{};
  BuildFrame(_outputBuffer, opcode, payload, fin, shouldMask, mask, false);
}

bool WebSocketHandler::sendData(Opcode opcode, std::span<const std::byte> payload) {
  if (_closeState != CloseState::Open) {
    return false;
  }

  // Try to compress if compression is enabled and payload is large enough
  if (_deflateContext && !_deflateContext->shouldSkipCompression(payload.size())) {
    // The previous payload sent from where it is may be in the compression buffer (and is queued first anyway).
    appendUnbufferedPayload();
    _compressBuffer.clear();
    const char* errMsg = _deflateContext->compress(payload, _compressBuffer);
    if (errMsg == nullptr && _compressBuffer.size() < payload.size()) {
      const bool shouldMask = !_config.isServerSide;
      const MaskingKey mask = shouldMask ? GenerateMaskingKey(_rngState) : MaskingKey{};
      if (_compressBuffer.size() < kMinUnbufferedPayloadSize) {
        BuildFrame(_outputBuffer, opcode, _compressBuffer, true, shouldMask, mask, true);
      } else {
        // Sent from the compression buffer (masked in place on the client side): only its frame header is buffered.
        if (shouldMask) {
          ApplyMask(std::span<std::byte>(_compressBuffer.data(), _compressBuffer.size()), mask);
        }
        BuildFrameHeader(_outputBuffer, opcode, _compressBuffer.size(), shouldMask, mask, true);
        _unbufferedPayload = _compressBuffer;
      }
      return true;
    }
  }

  // A large payload received in the processed input (an echo) is sent from there, without copying it: only its frame
  // header is buffered. Masked frames (client side) need their own copy of the payload.
  if (_config.isServerSide && payload.size() >= kMinUnbufferedPayloadSize && IsWithin(payload, _input)) {
    appendUnbufferedPayload();
    BuildFrameHeader(_outputBuffer, opcode, payload.size());
    _unbufferedPayload = payload;
    return true;
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

  appendUnbufferedPayload();
  const bool shouldMask = !_config.isServerSide;
  const MaskingKey mask = shouldMask ? GenerateMaskingKey(_rngState) : MaskingKey{};
  BuildCloseFrame(_outputBuffer, code, reason, shouldMask, mask);

  if (_closeState == CloseState::Open) {
    _closeState = CloseState::CloseSent;
    _closeInitiatedAt = std::chrono::steady_clock::now();
  }
  _closeCode = code;

  return true;
}

std::chrono::steady_clock::time_point WebSocketHandler::nextTimeoutCheck(
    std::chrono::steady_clock::time_point lastActivity) const noexcept {
  auto next = std::chrono::steady_clock::time_point::max();
  if (_closeState == CloseState::CloseSent) {
    next = _closeInitiatedAt + _config.closeTimeout;
  }
  if (_config.idleTimeout.count() > 0) {
    // Until a Ping is sent for the current idle period, the next check is the one that sends it.
    const bool pingDue = _config.autoPing && _closeState == CloseState::Open && _idlePingSentAt < lastActivity;
    next = std::min(next, lastActivity + (pingDue ? _config.idleTimeout / 2 : _config.idleTimeout));
  }
  return next;
}

WebSocketHandler::Timeout WebSocketHandler::checkTimeouts(std::chrono::steady_clock::time_point now,
                                                          std::chrono::steady_clock::time_point lastActivity) {
  if (_closeState == CloseState::CloseSent && now >= _closeInitiatedAt + _config.closeTimeout) {
    return Timeout::Close;
  }
  if (_config.idleTimeout.count() > 0) {
    if (now >= lastActivity + _config.idleTimeout) {
      return Timeout::Idle;
    }
    if (_config.autoPing && _idlePingSentAt < lastActivity && now >= lastActivity + (_config.idleTimeout / 2) &&
        sendPing()) {
      _idlePingSentAt = now;
    }
  }
  return Timeout::None;
}

std::unique_ptr<WebSocketHandler> CreateServerWebSocketHandler(WebSocketCallbacks callbacks,
                                                               std::size_t maxMessageSize) {
  WebSocketConfig config;
  config.isServerSide = true;
  config.maxMessageSize = maxMessageSize;
  return std::make_unique<WebSocketHandler>(std::move(config), std::move(callbacks));
}

std::unique_ptr<WebSocketHandler> CreateClientWebSocketHandler(WebSocketCallbacks callbacks,
                                                               std::size_t maxMessageSize) {
  WebSocketConfig config;
  config.isServerSide = false;
  config.maxMessageSize = maxMessageSize;
  return std::make_unique<WebSocketHandler>(std::move(config), std::move(callbacks));
}

}  // namespace aeronet::websocket
