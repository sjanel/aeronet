#include "aeronet/websocket-handler.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/chrono-lite.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/secure-random.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/websocket-config.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-frame.hpp"
#include "aeronet/websocket-utf8.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/websocket-deflate-context.hpp"
#include "aeronet/websocket-deflate.hpp"
#endif

namespace aeronet::websocket {

namespace {

// Smallest payload sent from where it is (the received input, the compression buffer) instead of being copied to the
// output buffer: below, the copy is cheaper than the separate buffer to write.
constexpr std::size_t kMinUnbufferedPayloadSize = 4096;

// Largest capacity a buffer of the received messages keeps once a message is done with. A larger one is freed: else a
// peer could leave maxMessageSize bytes allocated per connection, and with permessage-deflate from a few kilobytes of
// compressed data (a compression bomb). Of the order of the zlib streams of a connection (32 KiB windows).
constexpr std::size_t kMaxRetainedBufferCapacity = 64UL * 1024UL;

// Tells whether 'part' lies within 'whole' (std::less_equal is a total order, even for unrelated pointers).
bool IsWithin(std::span<const std::byte> part, std::span<const std::byte> whole) {
  constexpr std::less_equal<> kLessEqual;
  return kLessEqual(whole.data(), part.data()) && kLessEqual(part.data() + part.size(), whole.data() + whole.size());
}

// Frees the memory of 'buffer', whose content is not needed anymore, if it exceeds kMaxRetainedBufferCapacity.
void ReleaseIfOversized(RawBytes& buffer) noexcept {
  if (buffer.capacity() > kMaxRetainedBufferCapacity) {
    buffer = RawBytes();
  }
}

}  // namespace

// The masking keys must be unpredictable: a key guessed from the previous ones lets a script choose the bytes that an
// intermediary sees on the wire (RFC 6455 §5.3, §10.3). They are drawn from the secure generator of the system, by
// batches: one system call per kNbKeys frames.
struct WebSocketHandler::MaskingKeys {
  static constexpr uint32_t kNbKeys = 64;

  std::array<MaskingKey, kNbKeys> keys;
  uint32_t nextPos{kNbKeys};
};

WebSocketHandler::WebSocketHandler(WebSocketConfig config, WebSocketCallbacks callbacks
#ifdef AERONET_ENABLE_ZLIB
                                   ,
                                   DeflateNegotiatedParams deflateParams
#endif
                                   )
    : _config(std::move(config)), _callbacks(std::move(callbacks)) {
  _config.validate();
#ifdef AERONET_ENABLE_ZLIB
  if (deflateParams.hasValue()) {
    enableCompression(deflateParams);
  }
#endif
}

WebSocketHandler::WebSocketHandler(WebSocketHandler&&) noexcept = default;
WebSocketHandler& WebSocketHandler::operator=(WebSocketHandler&&) noexcept = default;

WebSocketHandler::~WebSocketHandler() = default;

void WebSocketHandler::setCallbacks(WebSocketCallbacks callbacks) { _callbacks = std::move(callbacks); }

#ifdef AERONET_ENABLE_ZLIB
void WebSocketHandler::enableCompression(DeflateNegotiatedParams deflateParams, SharedDeflateStreams* sharedStreams) {
  _deflateContext =
      std::make_unique<DeflateContext>(deflateParams, _config.deflateConfig, _config.isServerSide, sharedStreams);
}
#endif

ProtocolProcessResult WebSocketHandler::processInput(std::span<const std::byte> data) {
  // Only complete frames are consumed. The bytes of a trailing incomplete frame are left to the caller, which passes
  // them again, followed by the next received bytes, at the next call (they are not copied here).
  ProtocolProcessResult result;
#ifdef AERONET_ENABLE_ZLIB
  const bool allowRsv1 = (_deflateContext != nullptr);
#else
  constexpr bool allowRsv1 = false;
#endif

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

  const std::size_t inputSize = data.size();
  // Once the connection ended (onClose or onError called), the loop stops.
  while (_end == End::None && !data.empty()) {
    const auto frameResult = ParseFrame(data, _config.maxFrameSize, _config.isServerSide, allowRsv1);

    if (frameResult.status == FrameParseResult::Status::Incomplete) {
      break;
    }

    if (frameResult.status == FrameParseResult::Status::ProtocolError) {
      result.action = failConnection(CloseCode::ProtocolError, frameResult.pErrorMessage).action;
      break;
    }

    if (frameResult.status == FrameParseResult::Status::PayloadTooLarge) {
      result.action = failConnection(CloseCode::MessageTooBig, "Frame payload too large").action;
      break;
    }

    const ProtocolProcessResult frameProcessResult = processFrame(frameResult);

    result.bytesConsumed += frameResult.bytesConsumed;
    data = data.subspan(frameResult.bytesConsumed);

    // processFrame() never returns CloseImmediate in the WebSocket protocol (only HTTP/2 does), and Close only once the
    // connection ended.
    assert(frameProcessResult.action != ProtocolProcessResult::Action::CloseImmediate);
    assert(frameProcessResult.action != ProtocolProcessResult::Action::Close || _end == End::Reported);
    if (frameProcessResult.action != ProtocolProcessResult::Action::Continue) {
      result.action = frameProcessResult.action;
    }
  }

  if (_end != End::None) {
    // The closing handshake is complete or the connection failed: nothing more of the peer is processed (RFC 6455
    // §5.5.1, §7.1.7), its bytes are discarded. A client answering a Close frame gets ResponseReady, to send its own.
    result.bytesConsumed = inputSize;
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
      return failConnection(CloseCode::ProtocolError, "Unexpected continuation frame");
    }
  } else {
    // Text or Binary - must NOT be in a fragmented message
    if (_message.inProgress) {
      return failConnection(CloseCode::ProtocolError, "Expected continuation frame");
    }

    // Fast path: single complete non-fragmented non-compressed frame.
    // Bypass the message reassembly buffer entirely — unmask the payload in-place
    // and deliver directly to the callback without any extra allocation or copy.
    // Safety: 'payload' is a view into the caller's TCP receive buffer (pState->inBuffer),
    // which is mutable memory cast to const at the call site. Modifying it here is safe
    // because the buffer will be consumed (erased) immediately after processInput returns.
    // With permessage-deflate, RSV1 tells whether the message is compressed (only allowed when negotiated).
    if (header.fin && !header.rsv1) {
      if (payload.size() > _config.maxMessageSize) {
        return failConnection(CloseCode::MessageTooBig, "Message too large");
      }
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
      const std::span<std::byte> mutablePayload(const_cast<std::byte*>(payload.data()), payload.size());
      if (header.opcode == Opcode::Text) {
        // A masked text message is unmasked in the same pass as its UTF-8 is validated.
        const bool isValidUtf8 =
            header.masked ? UnmaskAndValidateUtf8(mutablePayload, header.maskingKey) : IsValidUtf8(payload);
        if (!isValidUtf8) {
          return failConnection(CloseCode::InvalidPayloadData, "Invalid UTF-8 in text message");
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
    _message.utf8TailSize = 0;

    // Per RFC 7692: RSV1 is set only on the first frame of a compressed message
    _messageCompressed = header.rsv1;
  }

  // Check message size limit (fragmented / compressed path)
  const std::size_t newSize = _message.buffer.size() + payload.size();
  if (newSize > _config.maxMessageSize) {
    return failConnection(CloseCode::MessageTooBig, "Message too large");
  }

  // Append payload to message buffer, unmasked in the same pass as copied
  if (header.masked) {
    _message.buffer.ensureAvailableCapacityExponential(payload.size());
    _message.buffer.setEnd(AppendMasked(payload, _message.buffer.data() + _message.buffer.size(), header.maskingKey));
  } else {
    _message.buffer.append(payload);
  }

  // An uncompressed text message is validated fragment by fragment: invalid UTF-8 fails the connection at once (fail
  // fast, RFC 6455 §8.1), without buffering the rest of the message. The incomplete code point that may end a fragment
  // is validated again with the start of the next one.
  if (_message.opcode == Opcode::Text && !_messageCompressed) {
    const auto unvalidated = std::span<const std::byte>(_message.buffer).last(payload.size() + _message.utf8TailSize);
    bool isValidUtf8 = true;
    if (header.fin) {
      isValidUtf8 = IsValidUtf8(unvalidated);
    } else {
      const std::size_t tailSize = ValidateUtf8Prefix(unvalidated);
      isValidUtf8 = tailSize != kInvalidUtf8Prefix;
      _message.utf8TailSize = static_cast<uint8_t>(tailSize);
    }
    if (!isValidUtf8) {
      return failConnection(CloseCode::InvalidPayloadData, "Invalid UTF-8 in text message");
    }
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
      if (closeInfo.pErrorMessage != nullptr) {
        // Invalid Close frame (code that may not be sent, reason that is not UTF-8...): not echoed
        return failConnection(closeInfo.code, closeInfo.pErrorMessage);
      }

      // The input is not processed anymore (the loop of processInput() stops here).
      assert(_closeState == CloseState::Open || _closeState == CloseState::CloseSent);
      _end = End::Reported;
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
      } else {
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

#ifdef AERONET_ENABLE_ZLIB
  // Decompress if message was compressed
  if (_messageCompressed && _deflateContext) {
    // The compressed payload of a message sent earlier may still wait in the compression buffer, written to below.
    if (!unbufferedPayloadInInput()) {
      appendUnbufferedPayload();
    }
    _compressBuffer.clear();
    const char* errMsg = _deflateContext->decompress(_message.buffer, _compressBuffer, _config.maxMessageSize);
    if (errMsg != nullptr) {
      // A message decompressing beyond maxMessageSize is too big, not invalid: its output then holds more bytes.
      return _compressBuffer.size() > _config.maxMessageSize
                 ? failConnection(CloseCode::MessageTooBig, "Message too large")
                 : failConnection(CloseCode::InvalidPayloadData, "Decompression failed");
    }
    // The message buffer holds the decompressed message: the compression buffer must stay free for the messages that
    // onMessage sends (an echo of this one would otherwise be compressed into its own memory).
    _message.buffer.swap(_compressBuffer);
  }
#endif

  const std::span<const std::byte> messageData = _message.buffer;

  // A decompressed text message is validated whole (an uncompressed one was, fragment by fragment)
  if (_message.opcode == Opcode::Text && _messageCompressed && !IsValidUtf8(messageData)) {
    return failConnection(CloseCode::InvalidPayloadData, "Invalid UTF-8 in text message");
  }

  if (_callbacks.onMessage) {
    _callbacks.onMessage(messageData, _message.opcode == Opcode::Binary);
  }

  resetMessage();

  result.action = ProtocolProcessResult::Action::Continue;
  return result;
}

void WebSocketHandler::resetMessage() noexcept {
  _message.inProgress = false;
  _message.buffer.clear();
  _messageCompressed = false;
  ReleaseIfOversized(_message.buffer);
#ifdef AERONET_ENABLE_ZLIB
  // The decompressed message or its compressed data, unless it holds the payload of a message being sent (sendData()).
  if (_unbufferedPayload.empty() || unbufferedPayloadInInput()) {
    ReleaseIfOversized(_compressBuffer);
  }
#endif
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
#ifdef AERONET_ENABLE_ZLIB
  return !_unbufferedPayload.empty() && !IsWithin(_unbufferedPayload, _compressBuffer);
#else
  // Without compression buffer, a payload sent from where it is can only be in the processed input.
  return !_unbufferedPayload.empty();
#endif
}

void WebSocketHandler::releaseInput() {
  // A payload in the compression buffer stays there: only the next write to this buffer needs to copy it.
  if (unbufferedPayloadInInput()) {
    appendUnbufferedPayload();
  }
}

void WebSocketHandler::onTransportClosing() {
  _closeState = CloseState::Closed;  // nothing can be sent anymore, onClose included
  _unbufferedPayload = {};
  resetMessage();
  if (_end != End::Reported) {
    // Closed without closing handshake: AbnormalClosure, the code reported when no Close frame was received (RFC 6455
    // §7.1.5).
    _end = End::Reported;
    if (_callbacks.onClose) {
      _callbacks.onClose(CloseCode::AbnormalClosure, "connection closed without closing handshake");
    }
  }
}

void WebSocketHandler::onInputProcessingException() {
  // What the callbacks did with the input is unknown: nothing more of it is processed, and the peer learns that the
  // server failed (RFC 6455 §7.4.1). The frames already queued are sent before the Close frame.
  if (_end == End::None) {
    _end = End::InputIgnored;
  }
  resetMessage();
  sendClose(CloseCode::InternalError);
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

MaskingKey WebSocketHandler::nextMaskingKey() {
  if (!_maskingKeys) {
    _maskingKeys = std::make_unique<MaskingKeys>();
  }
  MaskingKeys& pool = *_maskingKeys;
  if (pool.nextPos == MaskingKeys::kNbKeys) {
    FillSecureRandomBytes(std::as_writable_bytes(std::span(pool.keys)));
    pool.nextPos = 0;
  }
  return pool.keys[pool.nextPos++];
}

void WebSocketHandler::queueFrame(Opcode opcode, std::span<const std::byte> payload, bool fin) {
  appendUnbufferedPayload();
  const bool shouldMask = !_config.isServerSide;
  const MaskingKey mask = shouldMask ? nextMaskingKey() : MaskingKey{};
  BuildFrame(_outputBuffer, opcode, payload, fin, shouldMask, mask, false);
}

bool WebSocketHandler::sendText(std::string_view text) {
  const auto payload = std::as_bytes(std::span(text));
  assert(IsValidUtf8(payload));
  return sendData(Opcode::Text, payload);
}

bool WebSocketHandler::sendData(Opcode opcode, std::span<const std::byte> payload) {
  if (_closeState != CloseState::Open) {
    return false;
  }

#ifdef AERONET_ENABLE_ZLIB
  // Try to compress if compression is enabled and payload is large enough
  if (_deflateContext && !_deflateContext->shouldSkipCompression(payload.size())) {
    // The previous payload sent from where it is may be in the compression buffer (and is queued first anyway).
    appendUnbufferedPayload();
    _compressBuffer.clear();
    const char* errMsg = _deflateContext->compress(payload, _compressBuffer);
    // A message that does not shrink is sent uncompressed, unless the compression keeps its context: the window of the
    // peer must then hold it, the next messages referring to it. A failed compression stream keeps failing: the
    // messages sent uncompressed from then on refer to nothing.
    if (errMsg == nullptr && (_compressBuffer.size() < payload.size() || _deflateContext->keepsCompressionContext())) {
      const bool shouldMask = !_config.isServerSide;
      const MaskingKey mask = shouldMask ? nextMaskingKey() : MaskingKey{};
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
#endif

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

ProtocolProcessResult WebSocketHandler::failConnection(CloseCode code, std::string_view message) {
  // The input is not processed anymore (the loop of processInput() stops).
  assert(_end == End::None);
  _end = End::Reported;
  if (_callbacks.onError) {
    _callbacks.onError(code, message);
  }
  sendClose(code, message);
  resetMessage();
  ProtocolProcessResult result;
  result.action = ProtocolProcessResult::Action::Close;
  return result;
}

bool WebSocketHandler::sendClose(CloseCode code, std::string_view reason) {
  if (_closeState == CloseState::CloseSent || _closeState == CloseState::Closed) {
    return false;
  }

  appendUnbufferedPayload();
  const bool shouldMask = !_config.isServerSide;
  const MaskingKey mask = shouldMask ? nextMaskingKey() : MaskingKey{};
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
    next = DeadlineAfter(_closeInitiatedAt, _config.closeTimeout);
  }
  // Until a Ping is sent for the current idle period, the next check is the one that sends it.
  const bool pingDue = _config.autoPing && _closeState == CloseState::Open && _idlePingSentAt < lastActivity;
  return std::min(next, DeadlineAfter(lastActivity, pingDue ? _config.idleTimeout / 2 : _config.idleTimeout));
}

bool WebSocketHandler::onSweep(std::chrono::steady_clock::time_point now, bool draining) {
  // A drain closes the connection like the server's shutdown would (RFC 6455 §7.4.1), instead of the abnormal closure
  // of its deadline. Once the peer answered, or closeTimeout later, the connection is closed: the drain may then end
  // before its deadline.
  if (draining && _closeState == CloseState::Open) {
    sendClose(CloseCode::GoingAway, "Server shutting down");
  }
  return !hasCloseTimedOut(now);
}

WebSocketHandler::Timeout WebSocketHandler::checkTimeouts(std::chrono::steady_clock::time_point now,
                                                          std::chrono::steady_clock::time_point lastActivity) {
  if (hasCloseTimedOut(now)) {
    return Timeout::Close;
  }
  if (now >= DeadlineAfter(lastActivity, _config.idleTimeout)) {
    return Timeout::Idle;
  }
  if (_config.autoPing && _idlePingSentAt < lastActivity &&
      now >= DeadlineAfter(lastActivity, _config.idleTimeout / 2) && sendPing()) {
    _idlePingSentAt = now;
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
