#include "aeronet/http2-connection.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/http2-process-result-error-msg.hpp"
#include "aeronet/http2-stream.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/tolower-str.hpp"
#include "http2-read-write.hpp"

namespace aeronet::http2 {

namespace {

constexpr std::size_t kConnectionPrefaceLength = kConnectionPreface.size();
constexpr std::size_t kClosedStreamsMaxRetained = 16;

// Security hardening (CVE-2024-27316 mitigation): limit the total accumulated
// header block size across HEADERS + CONTINUATION frames to prevent unbounded
// memory growth from a CONTINUATION flood attack.
constexpr std::size_t kMaxHeaderBlockAccumulationSize = 256UL * 1024;

// Security hardening: cap PRIORITY frames received on non-existent streams
// to prevent an attacker from flooding cheap PRIORITY frames that starve
// real request processing. ENHANCE_YOUR_CALM is sent when exceeded.
constexpr uint32_t kMaxIdlePriorityFrames = 10000U;

constexpr uint32_t kMinMaxFrameSize = 16384;     // Minimum allowed SETTINGS_MAX_FRAME_SIZE
constexpr uint32_t kMaxMaxFrameSize = 16777215;  // Maximum allowed SETTINGS_MAX_FRAME_SIZE

}  // namespace

// ============================
// Constructor / Destructor
// ============================

Http2Connection::Http2Connection(const Http2Config& config, bool isServer)
    : _localSettings(config),
      _connectionSendWindow(static_cast<int32_t>(kDefaultInitialWindowSize)),
      _connectionRecvWindow(static_cast<int32_t>(config.connectionWindowSize)),
      _hpackEncoder(config.headerTableSize),
      _hpackDecoder(config.headerTableSize, config.mergeUnknownRequestHeaders),
      // Reserve some initial space for output buffer
      _outputBuffer(1024),
      _isServer(isServer) {}

// ============================
// Connection lifecycle
// ============================

Http2Connection::ProcessResult Http2Connection::processInput(std::span<const std::byte> data) {
  if (data.empty()) {
    return ProcessResult{ProcessResult::Action::Continue};
  }

  switch (_state) {
    case ConnectionState::AwaitingPreface:
      return processPreface(data);

    case ConnectionState::AwaitingSettings:
      [[fallthrough]];
    case ConnectionState::Open:
      [[fallthrough]];
    case ConnectionState::GoAwaySent:
      [[fallthrough]];
    case ConnectionState::GoAwayReceived:
      return processFrames(data);

    default:
      assert(_state == ConnectionState::Closed);
      return ProcessResult{ProcessResult::Action::Closed};
  }
}

void Http2Connection::onOutputWritten(std::size_t bytesWritten) {
  _outputWritePos += bytesWritten;

  // Reset buffer when fully consumed
  if (_outputWritePos >= _outputBuffer.size()) {
    _outputBuffer.clear();
    _outputWritePos = 0;
  }
}

void Http2Connection::initiateGoAway(ErrorCode errorCode, ErrorMsg msg) {
  if (_state == ConnectionState::Closed || _state == ConnectionState::GoAwaySent) {
    return;
  }

  WriteGoAwayFrame(_outputBuffer, _lastPeerStreamId, errorCode, msg);
  _state = ConnectionState::GoAwaySent;
  _goAwayLastStreamId = _lastPeerStreamId;
}

void Http2Connection::sendServerPreface() {
  // Only send if we're a server and haven't sent SETTINGS yet
  if (!_isServer || _settingsSent) {
    return;
  }

  // For TLS ALPN "h2", the server sends SETTINGS immediately without waiting for client preface.
  // However, we keep state as AwaitingPreface because we still need to receive and validate
  // the client's connection preface ("PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n").
  // The difference from h2c is just the order: for h2, server sends SETTINGS first.
  sendSettings();
}

void Http2Connection::sendClientPreface() {
  // Only send if we're a client and haven't sent SETTINGS yet
  if (_isServer || _settingsSent) {
    return;
  }

  // Write the client connection preface magic string
  _outputBuffer.append(reinterpret_cast<const std::byte*>(kConnectionPreface.data()), kConnectionPrefaceLength);

  // Send SETTINGS frame
  sendSettings();

  // Move to awaiting settings (waiting for server's SETTINGS)
  _state = ConnectionState::AwaitingSettings;
}

// ============================
// Stream management
// ============================

Http2Stream* Http2Connection::getStream(uint32_t streamId) noexcept {
  auto iter = _streams.find(streamId);
  if (iter != _streams.end()) {
    return &iter->second;
  }
  return nullptr;
}

void Http2Connection::closeStream(StreamsMap::iterator it, ErrorCode errorCode) {
  assert(it->second.isClosed());

  if (!it->second.markClosedNotified()) {
    return;
  }

  log::debug("Stream {} is now closed with error code {}", it->first, static_cast<uint32_t>(errorCode));
  if (errorCode != ErrorCode::NoError) {
    it->second.setErrorCode(errorCode);
  }
  assert(_activeStreamCount != 0);
  --_activeStreamCount;

  if (_onStreamClosed) {
    _onStreamClosed(it->first);
  }

  // Don't remove immediately - keep for a short time for late frames.
  _closedStreamsFifo.push_back(it->first);
  pruneClosedStreams();
}

void Http2Connection::pruneClosedStreams() {
  while (_closedStreamsFifo.size() > kClosedStreamsMaxRetained) {
    const auto streamId = _closedStreamsFifo.front();
    _closedStreamsFifo.pop_front();

    auto it = _streams.find(streamId);
    assert(it != _streams.end());
    assert(it->second.isClosed());
    _streams.erase(it);
  }
}

// ============================
// Frame sending
// ============================

ErrorCode Http2Connection::prepareSendHeaders(uint32_t streamId, bool endStream) {
  auto [it, inserted] = _streams.try_emplace(streamId, streamId, _peerSettings.initialWindowSize);
  if (inserted) {
    // Created new stream
    if (!canCreateStreams()) {
      _streams.erase(it);
      return ErrorCode::RefusedStream;
    }

    ++_activeStreamCount;
  }
  Http2Stream* pStream = &it->second;

  // Transition stream state
  return pStream->onSendHeaders(endStream);
}

namespace {
std::size_t EstimateHpackSize(std::size_t headersViewSize, const ConcatenatedHeaders* pGlobalHeaders,
                              uint8_t pseudoHeaderReserve) {
  const std::size_t plainHeadersSize =
      headersViewSize + (pGlobalHeaders != nullptr
                             ? (pGlobalHeaders->fullSizeWithLastSep() - http::HeaderSep.size() - http::CRLF.size())
                             : 0);
  return FrameHeader::kSize + pseudoHeaderReserve + ((plainHeadersSize * 2) / 3);
}
}  // namespace

ErrorCode Http2Connection::sendRequestHeaders(uint32_t streamId, http::Method method, bool isTlsRequest,
                                              std::string_view target, std::string_view authority,
                                              HeadersView headersView, bool endStream,
                                              const ConcatenatedHeaders* pGlobalHeaders) {
  const ErrorCode err = prepareSendHeaders(streamId, endStream);
  if (err != ErrorCode::NoError) {
    return err;
  }

  _outputBuffer.ensureAvailableCapacityExponential(EstimateHpackSize(headersView.size(), pGlobalHeaders, 24UL));

  // Make the header block be written after the frame header
  _outputBuffer.addSize(FrameHeader::kSize);

  const auto oldSize = _outputBuffer.size();
  if (!target.empty()) {
    // pseudo headers for requests.
    _hpackEncoder.encode(_outputBuffer, http::PseudoHeaderMethod, http::MethodToStr(method));
    _hpackEncoder.encode(_outputBuffer, http::PseudoHeaderScheme, isTlsRequest ? "https" : "http");
    _hpackEncoder.encode(_outputBuffer, http::PseudoHeaderAuthority, authority);
    _hpackEncoder.encode(_outputBuffer, http::PseudoHeaderPath, target);
  }

  // Encode headers
  encodeHeaders(streamId, target.empty() ? http::StatusCode{} : http::MagicForHttpRequest, headersView, endStream,
                oldSize, pGlobalHeaders);

  return err;
}

ErrorCode Http2Connection::sendHeaders(uint32_t streamId, http::StatusCode statusCode, HeadersView headersView,
                                       bool endStream, const ConcatenatedHeaders* pGlobalHeaders) {
  const ErrorCode err = prepareSendHeaders(streamId, endStream);
  if (err != ErrorCode::NoError) {
    return err;
  }

  _outputBuffer.ensureAvailableCapacityExponential(EstimateHpackSize(headersView.size(), pGlobalHeaders, 4UL));

  // Make the header block be written after the frame header
  _outputBuffer.addSize(FrameHeader::kSize);
  const auto oldSize = _outputBuffer.size();

  // Encode headers
  encodeHeaders(streamId, statusCode, headersView, endStream, oldSize, pGlobalHeaders);

  return err;
}

ErrorCode Http2Connection::sendData(uint32_t streamId, std::span<const std::byte> data, bool endStream) {
  Http2Stream* pStream = getStream(streamId);
  if (pStream == nullptr) [[unlikely]] {
    return ErrorCode::ProtocolError;
  }

  if (!pStream->canSend()) {
    return ErrorCode::StreamClosed;
  }

  // Check flow control
  auto dataSize = static_cast<uint32_t>(data.size());
  if (!pStream->consumeSendWindow(dataSize)) {
    return ErrorCode::FlowControlError;
  }

  if (std::cmp_less(_connectionSendWindow, dataSize)) {
    // Restore stream window
    (void)pStream->increaseSendWindow(dataSize);
    return ErrorCode::FlowControlError;
  }
  _connectionSendWindow -= static_cast<int32_t>(dataSize);

  // canSend() above already ensures the stream is Open or HalfClosedRemote,
  // which are exactly the states onSendData() handles — so this cannot fail.
  [[maybe_unused]] ErrorCode err = pStream->onSendData(endStream);
  assert(err == ErrorCode::NoError);

  // Write frame (may need to split if larger than max frame size)
  if (data.empty()) {
    // Empty DATA frame (valid in HTTP/2), used e.g. to signal END_STREAM without payload.
    if (endStream) {
      WriteDataFrame(_outputBuffer, streamId, {}, true);
    }
  } else {
    for (std::size_t offset = 0; offset < data.size();) {
      const std::size_t chunkSize =
          std::min(data.size() - offset, static_cast<std::size_t>(_peerSettings.maxFrameSize));
      const bool isLast = (offset + chunkSize >= data.size());
      WriteDataFrame(_outputBuffer, streamId, data.subspan(offset, chunkSize), isLast && endStream);
      offset += chunkSize;
    }
  }

  return ErrorCode::NoError;
}

void Http2Connection::sendRstStream(uint32_t streamId, ErrorCode errorCode) {
  WriteRstStreamFrame(_outputBuffer, streamId, errorCode);

  const auto it = _streams.find(streamId);
  if (it != _streams.end()) {
    it->second.onSendRstStream();
    it->second.setErrorCode(errorCode);
    closeStream(it, errorCode);
    if (_onStreamReset) {
      _onStreamReset(streamId, errorCode);
    }
  }
}

void Http2Connection::finalizeSendClosedStream(uint32_t streamId) {
  const auto it = _streams.find(streamId);
  if (it != _streams.end() && it->second.isClosed()) {
    closeStream(it);
  }
}

void Http2Connection::sendWindowUpdate(uint32_t streamId, uint32_t increment) {
  WriteWindowUpdateFrame(_outputBuffer, streamId, increment);

  // Security hardening: check for recv-window overflow (must not exceed 2^31-1
  // per RFC 9113 §6.9.1), matching the overflow guard on the send-window side.
  if (streamId == 0) {
    int64_t newWindow = static_cast<int64_t>(_connectionRecvWindow) + static_cast<int64_t>(increment);
    _connectionRecvWindow =
        static_cast<int32_t>(std::min(newWindow, static_cast<int64_t>(std::numeric_limits<int32_t>::max())));
  } else {
    Http2Stream* pStream = getStream(streamId);
    if (pStream != nullptr) {
      // increaseRecvWindow now returns ErrorCode; on the send side (our own
      // WINDOW_UPDATE) an overflow should not happen, but clamp defensively.
      (void)pStream->increaseRecvWindow(increment);
    }
  }
}

// ============================
// Frame processing
// ============================

Http2Connection::ProcessResult Http2Connection::processPreface(std::span<const std::byte> data) {
  if (_isServer) {
    // Server expects client preface
    if (data.size() < kConnectionPrefaceLength) {
      return ProcessResult{ProcessResult::Action::Continue};
    }
    // Compare against the connection preface string
    std::string_view dataView(reinterpret_cast<const char*>(data.data()), kConnectionPrefaceLength);
    if (dataView != kConnectionPreface) [[unlikely]] {
      return connectionError(ErrorCode::ProtocolError, ErrorMsg::InvalidConnectionPreface);
    }

    _state = ConnectionState::AwaitingSettings;

    // Server sends its SETTINGS immediately after receiving preface (for h2c).
    // For h2 (TLS ALPN), SETTINGS may have already been sent via sendServerPreface(),
    // so we check _settingsSent to avoid sending twice.
    if (!_settingsSent) {
      sendSettings();
    }

    return ProcessResult{ProcessResult::Action::OutputReady, kConnectionPrefaceLength};
  }
  // Client-side: we would have sent preface first
  // For now, just move to awaiting settings
  _state = ConnectionState::AwaitingSettings;
  return ProcessResult{ProcessResult::Action::Continue};
}

Http2Connection::ProcessResult Http2Connection::processFrames(std::span<const std::byte> data) {
  std::size_t totalConsumed = 0;

  while (data.size() >= FrameHeader::kSize) {
    const FrameHeader header = ParseFrameHeader(data);

    // Check frame size limits
    if (header.length > _localSettings.maxFrameSize) {
      return connectionError(ErrorCode::FrameSizeError, ErrorMsg::FrameExceedsMaximumSize);
    }

    const std::size_t totalFrameSize = FrameHeader::kSize + header.length;
    if (data.size() < totalFrameSize) {
      // Need more data
      break;
    }

    const auto payload = data.subspan(FrameHeader::kSize, header.length);

    ProcessResult result = processFrame(header, payload);

    // The current frame was processed, include it in bytes consumed.
    totalConsumed += totalFrameSize;

    if (result.action != ProcessResult::Action::Continue && result.action != ProcessResult::Action::OutputReady) {
      result.bytesConsumed = totalConsumed;
      return result;
    }

    data = data.subspan(totalFrameSize);
  }

  return ProcessResult{hasPendingOutput() ? ProcessResult::Action::OutputReady : ProcessResult::Action::Continue,
                       totalConsumed};
}

Http2Connection::ProcessResult Http2Connection::processFrame(FrameHeader header, std::span<const std::byte> payload) {
  // CONTINUATION frames must follow HEADERS/PUSH_PROMISE
  if (_expectingContinuation && header.type != FrameType::Continuation) {
    return connectionError(ErrorCode::ProtocolError, ErrorMsg::ExpectedCONTINUATIONFrame);
  }

  switch (header.type) {
    case FrameType::Data:
      return handleDataFrame(header, payload);
    case FrameType::Headers:
      return handleHeadersFrame(header, payload);
    case FrameType::Priority:
      return handlePriorityFrame(header, payload);
    case FrameType::RstStream:
      return handleRstStreamFrame(header, payload);
    case FrameType::Settings:
      return handleSettingsFrame(header, payload);
    case FrameType::PushPromise:
      // Server doesn't receive PUSH_PROMISE, client-only
      return connectionError(ErrorCode::ProtocolError, ErrorMsg::UnexpectedPUSH_PROMISE);
    case FrameType::Ping:
      return handlePingFrame(header, payload);
    case FrameType::GoAway:
      return handleGoAwayFrame(header, payload);
    case FrameType::WindowUpdate:
      return handleWindowUpdateFrame(header, payload);
    case FrameType::Continuation:
      return handleContinuationFrame(header, payload);
    default:
      // Unknown frame types are ignored (RFC 9113 §4.1)
      log::warn("Ignoring unknown frame type {}", static_cast<uint32_t>(header.type));
      return ProcessResult{ProcessResult::Action::Continue};
  }
}

Http2Connection::ProcessResult Http2Connection::handleDataFrame(FrameHeader header,
                                                                std::span<const std::byte> payload) {
  if (header.streamId == 0) {
    return connectionError(ErrorCode::ProtocolError, ErrorMsg::DATAFrameOnStreamZero);
  }

  DataFrame frame;
  FrameParseResult parseResult = ParseDataFrame(header, payload, frame);
  if (parseResult != FrameParseResult::Ok) {
    if (parseResult == FrameParseResult::InvalidPadding) {
      return connectionError(ErrorCode::ProtocolError, ErrorMsg::InvalidPaddingInDATAFrame);
    }
    return connectionError(ErrorCode::FrameSizeError, ErrorMsg::InvalidDATAFrame);
  }

  // Flow control: count full payload including padding
  auto payloadSize = static_cast<int32_t>(payload.size());

  if (payloadSize > _connectionRecvWindow) {
    return connectionError(ErrorCode::FlowControlError, ErrorMsg::ConnectionFlowControlExceeded);
  }
  _connectionRecvWindow -= payloadSize;

  auto it = _streams.find(header.streamId);
  if (it == _streams.end()) {
    // Stream may have been reset
    return ProcessResult{ProcessResult::Action::Continue};
  }

  if (!it->second.canReceive()) {
    return streamError(header.streamId, ErrorCode::StreamClosed, ErrorMsg::DATAOnClosedStream);
  }

  if (!it->second.consumeRecvWindow(static_cast<uint32_t>(payloadSize))) {
    return streamError(header.streamId, ErrorCode::FlowControlError, ErrorMsg::StreamFlowControlExceeded);
  }

  // canReceive() above already ensures the stream is Open or HalfClosedLocal,
  // which are exactly the states onRecvData() handles — so this cannot fail.
  [[maybe_unused]] ErrorCode err = it->second.onRecvData(frame.endStream);
  assert(err == ErrorCode::NoError);

  // Invoke callback
  if (_onData) {
    _onData(header.streamId, frame.data, frame.endStream);
  }

  // Update flow control windows.
  // We restore the consumed bytes immediately to avoid stalling peers on large transfers.
  // This is especially important for tests/clients which expect the connection to keep
  // making progress without application-managed WINDOW_UPDATE.
  if (_onData && payloadSize > 0) {
    const auto increment = static_cast<uint32_t>(payloadSize);
    sendWindowUpdate(header.streamId, increment);
    sendWindowUpdate(0, increment);
  }

  if (frame.endStream && it->second.isClosed()) {
    closeStream(it);
  }

  return ProcessResult{ProcessResult::Action::Continue};
}

Http2Connection::ProcessResult Http2Connection::handleHeadersFrame(FrameHeader header,
                                                                   std::span<const std::byte> payload) {
  if (header.streamId == 0) [[unlikely]] {
    return connectionError(ErrorCode::ProtocolError, ErrorMsg::HEADERSFrameOnStreamZero);
  }

  // Check for GOAWAY - don't accept new streams
  if ((_state == ConnectionState::GoAwaySent || _state == ConnectionState::GoAwayReceived) &&
      header.streamId > _goAwayLastStreamId) {
    return ProcessResult{ProcessResult::Action::Continue};  // Ignore
  }

  HeadersFrame frame;
  FrameParseResult parseResult = ParseHeadersFrame(header, payload, frame);
  if (parseResult != FrameParseResult::Ok) [[unlikely]] {
    if (parseResult == FrameParseResult::InvalidPadding) {
      return connectionError(ErrorCode::ProtocolError, ErrorMsg::InvalidPaddingInHEADERSFrame);
    }
    return connectionError(ErrorCode::FrameSizeError, ErrorMsg::InvalidHEADERSFrame);
  }

  // Get or create stream

  auto [it, inserted] = _streams.try_emplace(header.streamId, header.streamId, _peerSettings.initialWindowSize);
  if (inserted) {
    // Validate stream ID
    if (_isServer) {
      // Client-initiated streams must be odd and increasing
      if ((header.streamId & 1) == 0) [[unlikely]] {
        _streams.erase(it);
        return connectionError(ErrorCode::ProtocolError, ErrorMsg::ServerInitiatedStreamIdFromClient);
      }
      if (header.streamId <= _lastPeerStreamId) [[unlikely]] {
        _streams.erase(it);
        return connectionError(ErrorCode::ProtocolError, ErrorMsg::StreamIdNotIncreasing);
      }
    }

    // For peer-created streams, enforce OUR max concurrent streams limit
    // (not _peerSettings which limits streams WE initiate).
    if (_activeStreamCount >= _localSettings.maxConcurrentStreams) [[unlikely]] {
      _streams.erase(it);
      return connectionError(ErrorCode::ProtocolError, ErrorMsg::MaxConcurrentStreamsExceeded);
    }

    ++_activeStreamCount;
    _lastPeerStreamId = header.streamId;
  }
  Http2Stream* pStream = &it->second;

  // Handle priority if present
  if (frame.hasPriority) {
    if (frame.streamDependency == header.streamId) [[unlikely]] {
      return streamError(header.streamId, ErrorCode::ProtocolError, ErrorMsg::StreamDependsOnItself);
    }
    pStream->setPriority(frame.streamDependency, frame.weight, frame.exclusive);
  }

  // Accumulate header block
  if (!frame.endHeaders) {
    // Security hardening (CVE-2024-27316): reject oversized initial header block fragment
    // to prevent unbounded memory growth via CONTINUATION flood.
    if (frame.headerBlockFragment.size() > kMaxHeaderBlockAccumulationSize) [[unlikely]] {
      return connectionError(ErrorCode::EnhanceYourCalm, ErrorMsg::HeaderBlockTooLarge);
    }
    _expectingContinuation = true;
    _headerBlockStreamId = header.streamId;
    _headerBlockEndStream = frame.endStream;
    _headerBlockBuffer.assign(frame.headerBlockFragment);
    return ProcessResult{ProcessResult::Action::Continue};
  }

  // Complete header block - decode and deliver
  ErrorCode err = pStream->onRecvHeaders(frame.endStream);
  if (err != ErrorCode::NoError) [[unlikely]] {
    return streamError(header.streamId, err, ErrorMsg::InvalidStreamStateForHEADERS);
  }

  // Decode headers
  auto headerSpan = std::span<const std::byte>(frame.headerBlockFragment);

  // Decode and emit headers via helper
  ErrorCode decodeErr = decodeAndEmitHeaders(header.streamId, headerSpan, frame.endStream);
  if (decodeErr != ErrorCode::NoError) [[unlikely]] {
    return connectionError(decodeErr, ErrorMsg::HPACKDecodingFailed);
  }

  if (frame.endStream && pStream->isClosed()) {
    closeStream(it);
  }

  return ProcessResult{ProcessResult::Action::Continue};
}

Http2Connection::ProcessResult Http2Connection::handlePriorityFrame(FrameHeader header,
                                                                    std::span<const std::byte> payload) {
  if (header.streamId == 0) [[unlikely]] {
    return connectionError(ErrorCode::ProtocolError, ErrorMsg::PRIORITYFrameOnStreamZero);
  }

  PriorityFrame frame;
  FrameParseResult parseResult = ParsePriorityFrame(header, payload, frame);
  if (parseResult != FrameParseResult::Ok) [[unlikely]] {
    return connectionError(ErrorCode::FrameSizeError, ErrorMsg::InvalidPRIORITYFrame);
  }

  if (frame.streamDependency == header.streamId) [[unlikely]] {
    return streamError(header.streamId, ErrorCode::ProtocolError, ErrorMsg::StreamDependsOnItself);
  }

  Http2Stream* pStream = getStream(header.streamId);
  if (pStream != nullptr) {
    pStream->setPriority(frame.streamDependency, frame.weight, frame.exclusive);
  } else {
    // Security hardening: rate-limit PRIORITY frames on non-existent streams to
    // prevent a flood of cheap PRIORITY frames from starving real request processing.
    ++_idlePriorityFrameCount;
    if (_idlePriorityFrameCount > kMaxIdlePriorityFrames) [[unlikely]] {
      return connectionError(ErrorCode::EnhanceYourCalm, ErrorMsg::TooManyPRIORITYFramesOnIdleStreams);
    }
  }
  // PRIORITY can be sent for idle streams (pre-allocation)

  return ProcessResult{ProcessResult::Action::Continue};
}

Http2Connection::ProcessResult Http2Connection::handleRstStreamFrame(FrameHeader header,
                                                                     std::span<const std::byte> payload) {
  if (header.streamId == 0) {
    return connectionError(ErrorCode::ProtocolError, ErrorMsg::RST_STREAMFrameOnStreamZero);
  }

  RstStreamFrame frame;
  FrameParseResult parseResult = ParseRstStreamFrame(header, payload, frame);
  if (parseResult != FrameParseResult::Ok) [[unlikely]] {
    return connectionError(ErrorCode::FrameSizeError, ErrorMsg::InvalidRST_STREAMFrame);
  }

  auto it = _streams.find(header.streamId);
  if (it != _streams.end()) {
    it->second.onRecvRstStream();
    it->second.setErrorCode(frame.errorCode);
    closeStream(it, frame.errorCode);
    if (_onStreamReset) {
      _onStreamReset(header.streamId, frame.errorCode);
    }
  }

  return ProcessResult{ProcessResult::Action::Continue};
}

Http2Connection::ProcessResult Http2Connection::handleSettingsFrame(FrameHeader header,
                                                                    std::span<const std::byte> payload) {
  if (header.streamId != 0) {
    return connectionError(ErrorCode::ProtocolError, ErrorMsg::SETTINGSFrameOnNonZeroStream);
  }

  const bool isAck = header.hasFlag(FrameFlags::SettingsAck);
  if (isAck) {
    if (!payload.empty()) {
      return connectionError(ErrorCode::FrameSizeError, ErrorMsg::InvalidSETTINGSFrame);
    }
    if (_state == ConnectionState::AwaitingSettings) {
      _state = ConnectionState::Open;
    }
    _settingsAckReceived = true;
    return ProcessResult{ProcessResult::Action::Continue};
  }

  if (payload.size() % 6 != 0) {
    return connectionError(ErrorCode::FrameSizeError, ErrorMsg::InvalidSETTINGSFrame);
  }

  // payload.size() is already bounded by SETTINGS_MAX_FRAME_SIZE, checked upstream during generic frame header parsing
  // (common protection for all types).
  const std::size_t numEntries = payload.size() / 6;

  static constexpr uint32_t kUnsetWindowSizeValue = std::numeric_limits<uint32_t>::max();

  static_assert(kUnsetWindowSizeValue > kMaxWindowSize, "kUnsetWindowSizeValue must be greater than kMaxWindowSize");

  uint32_t newInitialWindowSize = kUnsetWindowSizeValue;  // "last wins", applied only once

  for (std::size_t idx = 0; idx < numEntries; ++idx) {
    const std::byte* entry = payload.data() + (idx * 6);
    const SettingsParameter id = static_cast<SettingsParameter>(Read16BE(entry));
    const uint32_t value = Read32BE(entry + 2);

    switch (id) {
      case SettingsParameter::HeaderTableSize:
        _peerSettings.headerTableSize = value;
        _hpackEncoder.setMaxDynamicTableSize(value);
        break;
      case SettingsParameter::EnablePush:
        if (value > 1) {
          return connectionError(ErrorCode::ProtocolError, ErrorMsg::InvalidENABLE_PUSHValue);
        }
        _peerSettings.enablePush = (value == 1);
        break;
      case SettingsParameter::MaxConcurrentStreams:
        _peerSettings.maxConcurrentStreams = value;
        break;
      case SettingsParameter::InitialWindowSize:
        if (value > kMaxWindowSize) {
          return connectionError(ErrorCode::FlowControlError, ErrorMsg::InitialWindowSizeTooLarge);
        }
        newInitialWindowSize = value;  // not applied immediately
        break;
      case SettingsParameter::MaxFrameSize:
        if (value < kMinMaxFrameSize || value > kMaxMaxFrameSize) {
          return connectionError(ErrorCode::ProtocolError, ErrorMsg::InvalidMAX_FRAMESize);
        }
        _peerSettings.maxFrameSize = value;
        break;
      case SettingsParameter::MaxHeaderListSize:
        _peerSettings.maxHeaderListSize = value;
        break;
      default:
        log::warn("Ignoring unknown SETTINGS parameter ID {}", static_cast<int>(id));
        break;
    }
  }

  // A single pass over the streams, regardless of the number of occurrences in the frame.
  if (newInitialWindowSize != kUnsetWindowSizeValue) {
    for (auto& [id, stream] : _streams) {
      ErrorCode err = stream.updateInitialWindowSize(newInitialWindowSize);
      if (err != ErrorCode::NoError) {
        return connectionError(err, ErrorMsg::WindowSizeUpdateOverflow);
      }
    }
    _peerSettings.initialWindowSize = newInitialWindowSize;
  }

  sendSettingsAck();

  if (_state == ConnectionState::AwaitingSettings) {
    _state = ConnectionState::Open;
  }

  return ProcessResult{ProcessResult::Action::OutputReady};
}

Http2Connection::ProcessResult Http2Connection::handlePingFrame(FrameHeader header,
                                                                std::span<const std::byte> payload) {
  if (header.streamId != 0) [[unlikely]] {
    return connectionError(ErrorCode::ProtocolError, ErrorMsg::PINGFrameOnNonZeroStream);
  }

  PingFrame frame;
  FrameParseResult parseResult = ParsePingFrame(header, payload, frame);
  if (parseResult != FrameParseResult::Ok) [[unlikely]] {
    return connectionError(ErrorCode::FrameSizeError, ErrorMsg::InvalidPINGFrame);
  }

  if (!frame.isAck) {
    // Send PING response
    frame.isAck = true;
    WritePingFrame(_outputBuffer, frame);
    return ProcessResult{ProcessResult::Action::OutputReady};
  }

  // PING ACK received - could track RTT here
  return ProcessResult{ProcessResult::Action::Continue};
}

Http2Connection::ProcessResult Http2Connection::handleGoAwayFrame(FrameHeader header,
                                                                  std::span<const std::byte> payload) {
  if (header.streamId != 0) {
    return connectionError(ErrorCode::ProtocolError, ErrorMsg::GOAWAYFrameOnNonZeroStream);
  }

  GoAwayFrame frame;
  FrameParseResult parseResult = ParseGoAwayFrame(header, payload, frame);
  if (parseResult != FrameParseResult::Ok) [[unlikely]] {
    return connectionError(ErrorCode::FrameSizeError, ErrorMsg::InvalidGOAWAYFrame);
  }

  _state = ConnectionState::GoAwayReceived;
  _goAwayLastStreamId = frame.lastStreamId;

  if (_onGoAway) {
    std::string_view debugData(reinterpret_cast<const char*>(frame.debugData.data()), frame.debugData.size());
    _onGoAway(frame.lastStreamId, frame.errorCode, debugData);
  }

  return ProcessResult{ProcessResult::Action::GoAway};
}

Http2Connection::ProcessResult Http2Connection::handleWindowUpdateFrame(FrameHeader header,
                                                                        std::span<const std::byte> payload) {
  WindowUpdateFrame frame;
  FrameParseResult parseResult = ParseWindowUpdateFrame(payload, frame);
  if (parseResult != FrameParseResult::Ok) {
    return connectionError(ErrorCode::FrameSizeError, ErrorMsg::InvalidWINDOW_UPDATEFrame);
  }

  if (frame.windowSizeIncrement == 0) {
    if (header.streamId == 0) {
      return connectionError(ErrorCode::ProtocolError, ErrorMsg::ZeroWINDOW_UPDATEIncrementOnConnection);
    }
    return streamError(header.streamId, ErrorCode::ProtocolError, ErrorMsg::ZeroWINDOW_UPDATEIncrement);
  }

  if (header.streamId == 0) {
    // Connection-level
    int64_t newWindow = static_cast<int64_t>(_connectionSendWindow) + frame.windowSizeIncrement;
    if (std::cmp_greater(newWindow, kMaxWindowSize)) {
      return connectionError(ErrorCode::FlowControlError, ErrorMsg::ConnectionWindowOverflow);
    }
    _connectionSendWindow = static_cast<int32_t>(newWindow);
  } else {
    // Stream-level
    Http2Stream* pStream = getStream(header.streamId);
    if (pStream != nullptr) {
      ErrorCode err = pStream->increaseSendWindow(frame.windowSizeIncrement);
      if (err != ErrorCode::NoError) {
        return streamError(header.streamId, err, ErrorMsg::StreamWindowOverflow);
      }
    }
  }

  if (_onWindowUpdate) {
    _onWindowUpdate(header.streamId, frame.windowSizeIncrement);
  }

  return ProcessResult{ProcessResult::Action::Continue};
}

Http2Connection::ProcessResult Http2Connection::handleContinuationFrame(FrameHeader header,
                                                                        std::span<const std::byte> payload) {
  if (!_expectingContinuation) [[unlikely]] {
    return connectionError(ErrorCode::ProtocolError, ErrorMsg::UnexpectedCONTINUATIONFrame);
  }

  if (header.streamId != _headerBlockStreamId) [[unlikely]] {
    return connectionError(ErrorCode::ProtocolError, ErrorMsg::CONTINUATIONOnWrongStream);
  }

  ContinuationFrame frame;
  ParseContinuationFrame(header, payload, frame);

  // Security hardening (CVE-2024-27316): enforce a bound on the total accumulated
  // header block size to prevent CONTINUATION flood attacks that grow memory unboundedly.
  if (_headerBlockBuffer.size() + frame.headerBlockFragment.size() > kMaxHeaderBlockAccumulationSize) [[unlikely]] {
    return connectionError(ErrorCode::EnhanceYourCalm, ErrorMsg::HeaderBlockTooLarge);
  }

  // Append to header block buffer
  _headerBlockBuffer.append(frame.headerBlockFragment);

  if (!frame.endHeaders) {
    return ProcessResult{ProcessResult::Action::Continue};
  }

  // Complete header block
  _expectingContinuation = false;

  const auto it = _streams.find(_headerBlockStreamId);
  if (it == _streams.end()) [[unlikely]] {
    return connectionError(ErrorCode::InternalError, ErrorMsg::StreamNotFoundForCONTINUATION);
  }

  const ErrorCode err = it->second.onRecvHeaders(_headerBlockEndStream);
  if (err != ErrorCode::NoError) [[unlikely]] {
    return streamError(_headerBlockStreamId, err, ErrorMsg::InvalidStreamStateForHEADERS);
  }

  // Decode complete header block
  const auto headerSpan = std::span<const std::byte>(_headerBlockBuffer);

  // Decode and emit headers via helper
  ErrorCode decodeErr = decodeAndEmitHeaders(_headerBlockStreamId, headerSpan, _headerBlockEndStream);
  if (decodeErr != ErrorCode::NoError) [[unlikely]] {
    return connectionError(decodeErr, ErrorMsg::HPACKDecodingFailed);
  }

  if (_headerBlockEndStream && it->second.isClosed()) {
    closeStream(it);
  }

  _headerBlockBuffer.clear();
  _headerBlockStreamId = 0;

  return ProcessResult{ProcessResult::Action::Continue};
}

// ============================
// HPACK
// ============================

void Http2Connection::encodeHeaders(uint32_t streamId, http::StatusCode statusCode, HeadersView headersView,
                                    bool endStream, std::size_t oldSize, const ConcatenatedHeaders* pGlobalHeaders) {
  assert(statusCode == 0 || statusCode == http::MagicForHttpRequest || (statusCode >= 100 && statusCode <= 999));

  // Encode :status pseudo-header first if present
  if (statusCode >= 100) {
    char statusBuf[3];
    writeStatusCode(statusBuf, statusCode);
    const std::string_view statusStr(statusBuf, sizeof(statusBuf));
    _hpackEncoder.encode(_outputBuffer, http::PseudoHeaderStatus, statusStr);
  }

  // For requests, skip the Host header (we use :authority instead)
  bool skipHostHeader = statusCode == http::MagicForHttpRequest;
  for (const auto& [name, value] : headersView) {
    if (skipHostHeader) {
      assert(name == http::Host);
      skipHostHeader = false;
      continue;
    }

    // RFC 9113 §8.2.1: an HTTP/2 field value must not carry leading/trailing OWS (unlike HTTP/1.1, where
    // it is tolerated). The HTTP/1.1 serializer legitimately emits such OWS -- e.g. the compression codec
    // pads Content-Length with trailing spaces (see http-codec.cpp) -- so trim before HPACK-encoding, or a
    // strict peer (nghttp2/curl) rejects the field and RST_STREAMs. TrimOws fast-paths already-clean values.
    // TODO: avoid const_cast by bringing a non-const headersView.
    tolower(const_cast<char*>(name.data()), name.size());
    _hpackEncoder.encode(_outputBuffer, name, TrimOws(value));
  }

  if (pGlobalHeaders != nullptr) {
    for (std::string_view headerKeyVal : *pGlobalHeaders) {
      const auto colonPos = headerKeyVal.find(http::HeaderSep);
      assert(colonPos != std::string_view::npos);

      // Skip if already present in request-specific headers. We can use case sensitive search because global headers
      // are already lower-cased in the config, and request-specific headers are lower-cased above.
      std::string_view headerNameWithColon = headerKeyVal.substr(0, colonPos + http::HeaderSep.size());
      if (headersView.containsCaseSensitive(headerNameWithColon)) {
        // Skip if already present in request-specific headers
        continue;
      }

      const std::string_view name = headerKeyVal.substr(0, colonPos);
      // Global header names should have been validated in the config.
      assert(std::ranges::all_of(name, [](char ch) { return ch < 'A' || ch > 'Z'; }));
      _hpackEncoder.encode(_outputBuffer, name, TrimOws(headerKeyVal.substr(colonPos + http::HeaderSep.size())));
    }
  }

  const uint32_t headerBlockSize = static_cast<uint32_t>(_outputBuffer.size() - oldSize);
  const auto outputSizeBeforeHeaders = oldSize - FrameHeader::kSize;

  // Check if we need to split into CONTINUATION frames
  if (headerBlockSize <= _peerSettings.maxFrameSize) {
    static constexpr bool kEndHeaders = true;  // All headers fit in one HEADERS frame, so END_HEADERS is always true
    const auto flags = ComputeHeaderFrameFlags(endStream, kEndHeaders);
    // Write the HEADERS frame header directly into the reserved gap.
    // We must NOT use setSize() to shrink + WriteFrame() + addSize() here, because
    // with AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS, setSize() poisons the bytes being
    // "freed" (the HPACK-encoded data) with 0xFF before we can re-claim them.
    WriteFrameHeader(_outputBuffer.data() + outputSizeBeforeHeaders,
                     {headerBlockSize, FrameType::Headers, flags, streamId});
    return;
  }
  // We will have at least one CONTINUATION frame.
  // Let's start by computing the exact total size needed.
  std::size_t totalSize = 0;
  for (uint32_t offset = 0; offset < headerBlockSize;) {
    const auto chunkSize = std::min(headerBlockSize - offset, _peerSettings.maxFrameSize);

    totalSize += FrameHeader::kSize + chunkSize;
    offset += chunkSize;
  }

  // reserve enough capacity in output buffer (no more reallocations)
  const auto remainingHeaderBlockSize = headerBlockSize - _peerSettings.maxFrameSize;
  // IMPORTANT:
  // - The HPACK-encoded header block bytes currently live at [oldSize, oldSize + headerBlockSize).
  // - With AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS, both reallocUp() (in reserve/ensureCapacity)
  //   and setSize() (when shrinking) poison bytes that become logically unused with 0xFF.
  // - We must reserve capacity first (to prevent reallocUp from poisoning within [_size, _capacity)),
  //   then move the ENTIRE header block to the end of the reserved space BEFORE shrinking the size,
  //   so the memmove reads the data before setSize poisons it.
  _outputBuffer.reserve(outputSizeBeforeHeaders + totalSize + headerBlockSize);

  // Move the ENTIRE header block data to the end of the reserved space BEFORE shrinking.
  const auto savedHeaderBlock = _outputBuffer.data() + _outputBuffer.capacity() - headerBlockSize;
  std::memmove(savedHeaderBlock, _outputBuffer.data() + oldSize, headerBlockSize);

  // Now it's safe to rewind the buffer size — the HPACK data is safe at the end of capacity.
  _outputBuffer.setSize(outputSizeBeforeHeaders);

  // Write the HEADERS frame WITHOUT END_HEADERS (it will be on the last CONTINUATION)
  const auto headersFlags = ComputeHeaderFrameFlags(endStream, false);
  WriteFrame(_outputBuffer, FrameType::Headers, headersFlags, streamId, _peerSettings.maxFrameSize);
  // Copy the first chunk of the header block data right after the HEADERS frame header
  std::memcpy(_outputBuffer.end(), savedHeaderBlock, _peerSettings.maxFrameSize);
  _outputBuffer.addSize(_peerSettings.maxFrameSize);

  // Capture the remaining header block span (past the first chunk)
  std::span<const std::byte> remainingHeaderBlock(savedHeaderBlock + _peerSettings.maxFrameSize,
                                                  remainingHeaderBlockSize);

  // Write continuation frames
  for (std::remove_const_t<decltype(remainingHeaderBlockSize)> offset = 0; offset < remainingHeaderBlockSize;) {
    const auto chunkSize = std::min(remainingHeaderBlockSize - offset, _peerSettings.maxFrameSize);
    const bool isLast = (offset + chunkSize >= remainingHeaderBlockSize);
    const auto chunkSpan = remainingHeaderBlock.subspan(offset, chunkSize);

    WriteContinuationFrame(_outputBuffer, streamId, chunkSpan, isLast);

    offset += chunkSize;
  }
}

ErrorCode Http2Connection::decodeAndEmitHeaders(uint32_t streamId, std::span<const std::byte> headerBlock,
                                                bool endStream) {
  // Collect decoded headers into an intermediate storage. We always build decodedHeaders
  // because we will invoke the new decoded-headers callback if set. Note: We must copy
  // strings here because the HPACK dynamic table may evict entries during decode,
  // invalidating string_views that point to evicted entries.

  const auto decodeResult = _hpackDecoder.decode(headerBlock);

  if (!decodeResult.isSuccess()) {
    return ErrorCode::CompressionError;
  }

  // Call the decoded-headers callback if set (owned strings)
  if (static_cast<bool>(_onHeadersDecoded)) {
    _onHeadersDecoded(streamId, decodeResult.decodedHeaders, endStream);
  }

  return ErrorCode::NoError;
}

// ============================
// Output helpers
// ============================

void Http2Connection::sendSettings() {
  const SettingsEntry entries[]{
      SettingsEntry{SettingsParameter::HeaderTableSize, _localSettings.headerTableSize},
      SettingsEntry{SettingsParameter::EnablePush, static_cast<uint32_t>(_localSettings.enablePush)},
      SettingsEntry{SettingsParameter::MaxConcurrentStreams, _localSettings.maxConcurrentStreams},
      SettingsEntry{SettingsParameter::InitialWindowSize, _localSettings.initialWindowSize},
      SettingsEntry{SettingsParameter::MaxFrameSize, _localSettings.maxFrameSize},
      SettingsEntry{SettingsParameter::MaxHeaderListSize, _localSettings.maxHeaderListSize},
  };

  WriteSettingsFrame(_outputBuffer, entries);
  _settingsSent = true;

  // Also send connection-level WINDOW_UPDATE if needed
  if (_localSettings.connectionWindowSize > kDefaultInitialWindowSize) {
    uint32_t increment = _localSettings.connectionWindowSize - kDefaultInitialWindowSize;
    WriteWindowUpdateFrame(_outputBuffer, 0, increment);
  }
}

// ============================
// Error handling
// ============================

Http2Connection::ProcessResult Http2Connection::connectionError(ErrorCode code, ErrorMsg msg) {
  initiateGoAway(code, msg);
  _state = ConnectionState::Closed;

  return ProcessResult{ProcessResult::Action::Error, code, msg};
}

Http2Connection::ProcessResult Http2Connection::streamError(uint32_t streamId, ErrorCode code, ErrorMsg msg) {
  sendRstStream(streamId, code);

  return ProcessResult{ProcessResult::Action::OutputReady, code, msg};
}

}  // namespace aeronet::http2
