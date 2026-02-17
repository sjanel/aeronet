#include "aeronet/http2-connection.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <utility>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/http2-stream.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/simple-charconv.hpp"

namespace aeronet::http2 {

namespace {

constexpr std::size_t kConnectionPrefaceLength = kConnectionPreface.size();
constexpr std::size_t kClosedStreamsMaxRetained = 16;

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
    return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};
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

    case ConnectionState::Closed:
      return ProcessResult{ProcessResult::Action::Closed, ErrorCode::NoError, 0, {}};

    default:
      throw std::logic_error("Invalid connection state");  // should not happen
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

void Http2Connection::initiateGoAway(ErrorCode errorCode, std::string_view debugData) {
  if (_state == ConnectionState::Closed || _state == ConnectionState::GoAwaySent) {
    return;
  }

  WriteGoAwayFrame(_outputBuffer, _lastPeerStreamId, errorCode, debugData);
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

ErrorCode Http2Connection::sendHeaders(uint32_t streamId, http::StatusCode statusCode, HeadersView headersView,
                                       bool endStream, const ConcatenatedHeaders* pGlobalHeaders) {
  auto [it, inserted] = _streams.try_emplace(streamId, streamId, _peerSettings.initialWindowSize);
  if (inserted) {
    // Created new stream
    if (!canCreateStreams()) {
      _streams.erase(it);
      return ErrorCode::RefusedStream;
    }

    ++_activeStreamCount;
  }
  Http2Stream* stream = &it->second;

  // Transition stream state
  ErrorCode err = stream->onSendHeaders(endStream);
  if (err != ErrorCode::NoError) {
    return err;
  }

  // Encode headers
  encodeHeaders(streamId, statusCode, headersView, endStream, pGlobalHeaders);

  return ErrorCode::NoError;
}

ErrorCode Http2Connection::sendData(uint32_t streamId, std::span<const std::byte> data, bool endStream) {
  Http2Stream* stream = getStream(streamId);
  if (stream == nullptr) [[unlikely]] {
    return ErrorCode::ProtocolError;
  }

  if (!stream->canSend()) {
    return ErrorCode::StreamClosed;
  }

  // Check flow control
  auto dataSize = static_cast<uint32_t>(data.size());
  if (!stream->consumeSendWindow(dataSize)) {
    return ErrorCode::FlowControlError;
  }

  if (std::cmp_less(_connectionSendWindow, dataSize)) {
    // Restore stream window
    (void)stream->increaseSendWindow(dataSize);
    return ErrorCode::FlowControlError;
  }
  _connectionSendWindow -= static_cast<int32_t>(dataSize);

  // Transition stream state
  ErrorCode err = stream->onSendData(endStream);
  if (err != ErrorCode::NoError) [[unlikely]] {
    return err;
  }

  // Write frame (may need to split if larger than max frame size)
  std::size_t offset = 0;
  while (offset < data.size()) {
    std::size_t chunkSize = std::min(data.size() - offset, static_cast<std::size_t>(_peerSettings.maxFrameSize));
    bool isLast = (offset + chunkSize >= data.size());
    WriteDataFrame(_outputBuffer, streamId, data.subspan(offset, chunkSize), isLast && endStream);
    offset += chunkSize;
  }

  return ErrorCode::NoError;
}

void Http2Connection::sendRstStream(uint32_t streamId, ErrorCode errorCode) {
  WriteRstStreamFrame(_outputBuffer, streamId, errorCode);

  auto it = _streams.find(streamId);
  if (it != _streams.end()) {
    it->second.onSendRstStream();
    it->second.setErrorCode(errorCode);
    closeStream(it, errorCode);
    if (_onStreamReset) {
      _onStreamReset(streamId, errorCode);
    }
  }
}

void Http2Connection::sendPing(PingFrame pingFrame) { WritePingFrame(_outputBuffer, pingFrame); }

void Http2Connection::sendWindowUpdate(uint32_t streamId, uint32_t increment) {
  WriteWindowUpdateFrame(_outputBuffer, streamId, increment);

  if (streamId == 0) {
    _connectionRecvWindow += static_cast<int32_t>(increment);
  } else {
    Http2Stream* stream = getStream(streamId);
    if (stream != nullptr) {
      stream->increaseRecvWindow(increment);
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
      return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};
    }
    // Compare against the connection preface string
    std::string_view dataView(reinterpret_cast<const char*>(data.data()), kConnectionPrefaceLength);
    if (dataView != kConnectionPreface) [[unlikely]] {
      return connectionError(ErrorCode::ProtocolError, "Invalid connection preface");
    }

    _state = ConnectionState::AwaitingSettings;

    // Server sends its SETTINGS immediately after receiving preface (for h2c).
    // For h2 (TLS ALPN), SETTINGS may have already been sent via sendServerPreface(),
    // so we check _settingsSent to avoid sending twice.
    if (!_settingsSent) {
      sendSettings();
    }

    return ProcessResult{ProcessResult::Action::OutputReady, ErrorCode::NoError, kConnectionPrefaceLength, {}};
  }
  // Client-side: we would have sent preface first
  // For now, just move to awaiting settings
  _state = ConnectionState::AwaitingSettings;
  return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};
}

Http2Connection::ProcessResult Http2Connection::processFrames(std::span<const std::byte> data) {
  std::size_t totalConsumed = 0;

  while (data.size() >= FrameHeader::kSize) {
    FrameHeader header = ParseFrameHeader(data);

    // Check frame size limits
    if (header.length > _localSettings.maxFrameSize) {
      return connectionError(ErrorCode::FrameSizeError, "Frame exceeds maximum size");
    }

    std::size_t totalFrameSize = FrameHeader::kSize + header.length;
    if (data.size() < totalFrameSize) {
      // Need more data
      break;
    }

    auto payload = data.subspan(FrameHeader::kSize, header.length);

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
                       ErrorCode::NoError,
                       totalConsumed,
                       {}};
}

Http2Connection::ProcessResult Http2Connection::processFrame(FrameHeader header, std::span<const std::byte> payload) {
  // CONTINUATION frames must follow HEADERS/PUSH_PROMISE
  if (_expectingContinuation && header.type != FrameType::Continuation) [[unlikely]] {
    return connectionError(ErrorCode::ProtocolError, "Expected CONTINUATION frame");
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
      return connectionError(ErrorCode::ProtocolError, "Unexpected PUSH_PROMISE");
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
      return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};
  }
}

Http2Connection::ProcessResult Http2Connection::handleDataFrame(FrameHeader header,
                                                                std::span<const std::byte> payload) {
  if (header.streamId == 0) {
    return connectionError(ErrorCode::ProtocolError, "DATA frame on stream 0");
  }

  DataFrame frame;
  FrameParseResult parseResult = ParseDataFrame(header, payload, frame);
  if (parseResult != FrameParseResult::Ok) [[unlikely]] {
    if (parseResult == FrameParseResult::InvalidPadding) {
      return connectionError(ErrorCode::ProtocolError, "Invalid padding in DATA frame");
    }
    return connectionError(ErrorCode::FrameSizeError, "Invalid DATA frame");
  }

  // Flow control: count full payload including padding
  auto payloadSize = static_cast<int32_t>(payload.size());

  if (payloadSize > _connectionRecvWindow) {
    return connectionError(ErrorCode::FlowControlError, "Connection flow control exceeded");
  }
  _connectionRecvWindow -= payloadSize;

  auto it = _streams.find(header.streamId);
  if (it == _streams.end()) {
    // Stream may have been reset
    return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};
  }

  if (!it->second.canReceive()) {
    return streamError(header.streamId, ErrorCode::StreamClosed, "DATA on closed stream");
  }

  if (!it->second.consumeRecvWindow(static_cast<uint32_t>(payloadSize))) {
    return streamError(header.streamId, ErrorCode::FlowControlError, "Stream flow control exceeded");
  }

  ErrorCode err = it->second.onRecvData(frame.endStream);
  if (err != ErrorCode::NoError) [[unlikely]] {
    return streamError(header.streamId, err, "Invalid stream state for DATA");
  }

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

  return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};
}

Http2Connection::ProcessResult Http2Connection::handleHeadersFrame(FrameHeader header,
                                                                   std::span<const std::byte> payload) {
  if (header.streamId == 0) [[unlikely]] {
    return connectionError(ErrorCode::ProtocolError, "HEADERS frame on stream 0");
  }

  // Check for GOAWAY - don't accept new streams
  if ((_state == ConnectionState::GoAwaySent || _state == ConnectionState::GoAwayReceived) &&
      header.streamId > _goAwayLastStreamId) {
    return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};  // Ignore
  }

  HeadersFrame frame;
  FrameParseResult parseResult = ParseHeadersFrame(header, payload, frame);
  if (parseResult != FrameParseResult::Ok) [[unlikely]] {
    if (parseResult == FrameParseResult::InvalidPadding) {
      return connectionError(ErrorCode::ProtocolError, "Invalid padding in HEADERS frame");
    }
    return connectionError(ErrorCode::FrameSizeError, "Invalid HEADERS frame");
  }

  // Get or create stream

  auto [it, inserted] = _streams.try_emplace(header.streamId, header.streamId, _peerSettings.initialWindowSize);
  if (inserted) {
    // Validate stream ID
    if (_isServer) {
      // Client-initiated streams must be odd and increasing
      if ((header.streamId & 1) == 0) [[unlikely]] {
        _streams.erase(it);
        return connectionError(ErrorCode::ProtocolError, "Server-initiated stream ID from client");
      }
      if (header.streamId <= _lastPeerStreamId) [[unlikely]] {
        _streams.erase(it);
        return connectionError(ErrorCode::ProtocolError, "Stream ID not increasing");
      }
    }

    if (!canCreateStreams()) [[unlikely]] {
      _streams.erase(it);
      return connectionError(ErrorCode::ProtocolError, "Max concurrent streams exceeded");
    }

    ++_activeStreamCount;
    _lastPeerStreamId = header.streamId;
  }
  Http2Stream* stream = &it->second;

  // Handle priority if present
  if (frame.hasPriority) {
    if (frame.streamDependency == header.streamId) [[unlikely]] {
      return streamError(header.streamId, ErrorCode::ProtocolError, "Stream depends on itself");
    }
    stream->setPriority(frame.streamDependency, frame.weight, frame.exclusive);
  }

  // Accumulate header block
  if (!frame.endHeaders) {
    _expectingContinuation = true;
    _headerBlockStreamId = header.streamId;
    _headerBlockEndStream = frame.endStream;
    _headerBlockBuffer.assign(frame.headerBlockFragment);
    return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};
  }

  // Complete header block - decode and deliver
  ErrorCode err = stream->onRecvHeaders(frame.endStream);
  if (err != ErrorCode::NoError) [[unlikely]] {
    return streamError(header.streamId, err, "Invalid stream state for HEADERS");
  }

  // Decode headers
  auto headerSpan = std::span<const std::byte>(frame.headerBlockFragment);

  // Decode and emit headers via helper
  ErrorCode decodeErr = decodeAndEmitHeaders(header.streamId, headerSpan, frame.endStream);
  if (decodeErr != ErrorCode::NoError) [[unlikely]] {
    return connectionError(decodeErr, "HPACK decoding failed");
  }

  if (frame.endStream && stream->isClosed()) {
    closeStream(it);
  }

  return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};
}

Http2Connection::ProcessResult Http2Connection::handlePriorityFrame(FrameHeader header,
                                                                    std::span<const std::byte> payload) {
  if (header.streamId == 0) [[unlikely]] {
    return connectionError(ErrorCode::ProtocolError, "PRIORITY frame on stream 0");
  }

  PriorityFrame frame;
  FrameParseResult parseResult = ParsePriorityFrame(header, payload, frame);
  if (parseResult != FrameParseResult::Ok) [[unlikely]] {
    return connectionError(ErrorCode::FrameSizeError, "Invalid PRIORITY frame");
  }

  if (frame.streamDependency == header.streamId) [[unlikely]] {
    return streamError(header.streamId, ErrorCode::ProtocolError, "Stream depends on itself");
  }

  Http2Stream* stream = getStream(header.streamId);
  if (stream != nullptr) {
    stream->setPriority(frame.streamDependency, frame.weight, frame.exclusive);
  }
  // PRIORITY can be sent for idle streams (pre-allocation)

  return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};
}

Http2Connection::ProcessResult Http2Connection::handleRstStreamFrame(FrameHeader header,
                                                                     std::span<const std::byte> payload) {
  if (header.streamId == 0) {
    return connectionError(ErrorCode::ProtocolError, "RST_STREAM frame on stream 0");
  }

  RstStreamFrame frame;
  FrameParseResult parseResult = ParseRstStreamFrame(header, payload, frame);
  if (parseResult != FrameParseResult::Ok) [[unlikely]] {
    return connectionError(ErrorCode::FrameSizeError, "Invalid RST_STREAM frame");
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

  return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};
}

Http2Connection::ProcessResult Http2Connection::handleSettingsFrame(FrameHeader header,
                                                                    std::span<const std::byte> payload) {
  if (header.streamId != 0) [[unlikely]] {
    return connectionError(ErrorCode::ProtocolError, "SETTINGS frame on non-zero stream");
  }

  SettingsFrame frame;
  FrameParseResult parseResult = ParseSettingsFrame(header, payload, frame);
  if (parseResult != FrameParseResult::Ok) [[unlikely]] {
    return connectionError(ErrorCode::FrameSizeError, "Invalid SETTINGS frame");
  }

  if (frame.isAck) {
    if (_state == ConnectionState::AwaitingSettings) {
      _state = ConnectionState::Open;
    }
    _settingsAckReceived = true;
    return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};
  }

  // Apply settings
  for (std::size_t idx = 0; idx < frame.entryCount; ++idx) {
    const auto& entry = frame.entries[idx];
    switch (entry.id) {
      case SettingsParameter::HeaderTableSize:
        _peerSettings.headerTableSize = entry.value;
        _hpackEncoder.setMaxDynamicTableSize(entry.value);
        break;
      case SettingsParameter::EnablePush:
        if (entry.value > 1) [[unlikely]] {
          return connectionError(ErrorCode::ProtocolError, "Invalid ENABLE_PUSH value");
        }
        _peerSettings.enablePush = (entry.value == 1);
        break;
      case SettingsParameter::MaxConcurrentStreams:
        _peerSettings.maxConcurrentStreams = entry.value;
        break;
      case SettingsParameter::InitialWindowSize:
        if (entry.value > 0x7FFFFFFF) [[unlikely]] {
          return connectionError(ErrorCode::FlowControlError, "Initial window size too large");
        }
        // Update all existing streams
        for (auto& [id, stream] : _streams) {
          ErrorCode err = stream.updateInitialWindowSize(entry.value);
          if (err != ErrorCode::NoError) [[unlikely]] {
            return connectionError(err, "Window size update overflow");
          }
        }
        _peerSettings.initialWindowSize = entry.value;
        break;
      case SettingsParameter::MaxFrameSize:
        if (entry.value < 16384 || entry.value > 16777215) [[unlikely]] {
          return connectionError(ErrorCode::ProtocolError, "Invalid MAX_FRAME_SIZE");
        }
        _peerSettings.maxFrameSize = entry.value;
        break;
      case SettingsParameter::MaxHeaderListSize:
        _peerSettings.maxHeaderListSize = entry.value;
        break;
      default:
        log::warn("Ignoring unknown SETTINGS parameter ID {}", static_cast<int>(entry.id));
        break;
    }
  }

  // Send SETTINGS ACK
  sendSettingsAck();

  // If we were awaiting settings, now we're open
  if (_state == ConnectionState::AwaitingSettings) {
    _state = ConnectionState::Open;
  }

  return ProcessResult{ProcessResult::Action::OutputReady, ErrorCode::NoError, 0, {}};
}

Http2Connection::ProcessResult Http2Connection::handlePingFrame(FrameHeader header,
                                                                std::span<const std::byte> payload) {
  if (header.streamId != 0) [[unlikely]] {
    return connectionError(ErrorCode::ProtocolError, "PING frame on non-zero stream");
  }

  PingFrame frame;
  FrameParseResult parseResult = ParsePingFrame(header, payload, frame);
  if (parseResult != FrameParseResult::Ok) [[unlikely]] {
    return connectionError(ErrorCode::FrameSizeError, "Invalid PING frame");
  }

  if (!frame.isAck) {
    // Send PING response
    frame.isAck = true;
    WritePingFrame(_outputBuffer, frame);
    return ProcessResult{ProcessResult::Action::OutputReady, ErrorCode::NoError, 0, {}};
  }

  // PING ACK received - could track RTT here
  return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};
}

Http2Connection::ProcessResult Http2Connection::handleGoAwayFrame(FrameHeader header,
                                                                  std::span<const std::byte> payload) {
  if (header.streamId != 0) {
    return connectionError(ErrorCode::ProtocolError, "GOAWAY frame on non-zero stream");
  }

  GoAwayFrame frame;
  FrameParseResult parseResult = ParseGoAwayFrame(header, payload, frame);
  if (parseResult != FrameParseResult::Ok) [[unlikely]] {
    return connectionError(ErrorCode::FrameSizeError, "Invalid GOAWAY frame");
  }

  _state = ConnectionState::GoAwayReceived;
  _goAwayLastStreamId = frame.lastStreamId;

  if (_onGoAway) {
    std::string_view debugData(reinterpret_cast<const char*>(frame.debugData.data()), frame.debugData.size());
    _onGoAway(frame.lastStreamId, frame.errorCode, debugData);
  }

  return ProcessResult{ProcessResult::Action::GoAway, ErrorCode::NoError, 0, {}};
}

Http2Connection::ProcessResult Http2Connection::handleWindowUpdateFrame(FrameHeader header,
                                                                        std::span<const std::byte> payload) {
  WindowUpdateFrame frame;
  FrameParseResult parseResult = ParseWindowUpdateFrame(payload, frame);
  if (parseResult != FrameParseResult::Ok) [[unlikely]] {
    return connectionError(ErrorCode::FrameSizeError, "Invalid WINDOW_UPDATE frame");
  }

  if (frame.windowSizeIncrement == 0) {
    if (header.streamId == 0) [[unlikely]] {
      return connectionError(ErrorCode::ProtocolError, "Zero WINDOW_UPDATE increment on connection");
    }
    return streamError(header.streamId, ErrorCode::ProtocolError, "Zero WINDOW_UPDATE increment");
  }

  if (header.streamId == 0) {
    // Connection-level
    int64_t newWindow = static_cast<int64_t>(_connectionSendWindow) + frame.windowSizeIncrement;
    if (newWindow > 0x7FFFFFFF) [[unlikely]] {
      return connectionError(ErrorCode::FlowControlError, "Connection window overflow");
    }
    _connectionSendWindow = static_cast<int32_t>(newWindow);
  } else {
    // Stream-level
    Http2Stream* stream = getStream(header.streamId);
    if (stream != nullptr) {
      ErrorCode err = stream->increaseSendWindow(frame.windowSizeIncrement);
      if (err != ErrorCode::NoError) [[unlikely]] {
        return streamError(header.streamId, err, "Stream window overflow");
      }
    }
  }

  return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};
}

Http2Connection::ProcessResult Http2Connection::handleContinuationFrame(FrameHeader header,
                                                                        std::span<const std::byte> payload) {
  if (!_expectingContinuation) [[unlikely]] {
    return connectionError(ErrorCode::ProtocolError, "Unexpected CONTINUATION frame");
  }

  if (header.streamId != _headerBlockStreamId) [[unlikely]] {
    return connectionError(ErrorCode::ProtocolError, "CONTINUATION on wrong stream");
  }

  ContinuationFrame frame;
  ParseContinuationFrame(header, payload, frame);

  // Append to header block buffer
  _headerBlockBuffer.append(frame.headerBlockFragment);

  if (!frame.endHeaders) {
    return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};
  }

  // Complete header block
  _expectingContinuation = false;

  auto it = _streams.find(_headerBlockStreamId);
  if (it == _streams.end()) [[unlikely]] {
    return connectionError(ErrorCode::InternalError, "Stream not found for CONTINUATION");
  }

  ErrorCode err = it->second.onRecvHeaders(_headerBlockEndStream);
  if (err != ErrorCode::NoError) [[unlikely]] {
    return streamError(_headerBlockStreamId, err, "Invalid stream state for HEADERS");
  }

  // Decode complete header block
  auto headerSpan = std::span<const std::byte>(_headerBlockBuffer);

  // Decode and emit headers via helper
  ErrorCode decodeErr = decodeAndEmitHeaders(_headerBlockStreamId, headerSpan, _headerBlockEndStream);
  if (decodeErr != ErrorCode::NoError) [[unlikely]] {
    return connectionError(decodeErr, "HPACK decoding failed");
  }

  if (_headerBlockEndStream && it->second.isClosed()) {
    closeStream(it);
  }

  _headerBlockBuffer.clear();
  _headerBlockStreamId = 0;

  return ProcessResult{ProcessResult::Action::Continue, ErrorCode::NoError, 0, {}};
}

// ============================
// HPACK
// ============================

void Http2Connection::encodeHeaders(uint32_t streamId, http::StatusCode statusCode, HeadersView headersView,
                                    bool endStream, const ConcatenatedHeaders* pGlobalHeaders) {
  _outputBuffer.ensureAvailableCapacityExponential(FrameHeader::kSize + 512);  // Reserve some space

  // Make the header block be written after the frame header
  _outputBuffer.addSize(FrameHeader::kSize);
  const auto oldSize = _outputBuffer.size();

  // Encode :status pseudo-header first if present
  if (statusCode != 0) {
    assert(statusCode >= 100 && statusCode <= 999);
    char statusStr[3];
    write3(statusStr, statusCode);
    _hpackEncoder.encode(_outputBuffer, http::PseudoHeaderStatus, std::string_view(statusStr, sizeof(statusStr)));
  }
  for (const auto& [name, value] : headersView) {
    _hpackEncoder.encode(_outputBuffer, name, value);
  }
  if (pGlobalHeaders != nullptr) {
    for (std::string_view headerKeyVal : *pGlobalHeaders) {
      const auto colonPos = headerKeyVal.find(':');
      assert(colonPos != std::string_view::npos);
      const std::string_view name = headerKeyVal.substr(0, colonPos);
      // Skip if already present in request-specific headers
      if (std::ranges::any_of(headersView, [name](const auto& header) { return header.name == name; })) {
        continue;
      }
      _hpackEncoder.encode(_outputBuffer, name, headerKeyVal.substr(colonPos + http::HeaderSep.size()));
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
  for (uint32_t offset = 0; offset < remainingHeaderBlockSize;) {
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
  const std::array<SettingsEntry, 6> entries = {
      SettingsEntry{SettingsParameter::HeaderTableSize, _localSettings.headerTableSize},
      SettingsEntry{SettingsParameter::EnablePush, static_cast<uint32_t>(_localSettings.enablePush)},
      SettingsEntry{SettingsParameter::MaxConcurrentStreams, _localSettings.maxConcurrentStreams},
      SettingsEntry{SettingsParameter::InitialWindowSize, _localSettings.initialWindowSize},
      SettingsEntry{SettingsParameter::MaxFrameSize, _localSettings.maxFrameSize},
      SettingsEntry{SettingsParameter::MaxHeaderListSize, _localSettings.maxHeaderListSize}};

  WriteSettingsFrame(_outputBuffer, entries);
  _settingsSent = true;

  // Also send connection-level WINDOW_UPDATE if needed
  if (_localSettings.connectionWindowSize > kDefaultInitialWindowSize) {
    uint32_t increment = _localSettings.connectionWindowSize - kDefaultInitialWindowSize;
    WriteWindowUpdateFrame(_outputBuffer, 0, increment);
  }
}

void Http2Connection::sendSettingsAck() { WriteSettingsAckFrame(_outputBuffer); }

// ============================
// Error handling
// ============================

Http2Connection::ProcessResult Http2Connection::connectionError(ErrorCode code, const char* message) {
  initiateGoAway(code, message);
  _state = ConnectionState::Closed;

  return ProcessResult{ProcessResult::Action::Error, code, 0, message};
}

Http2Connection::ProcessResult Http2Connection::streamError(uint32_t streamId, ErrorCode code, const char* message) {
  sendRstStream(streamId, code);

  return ProcessResult{ProcessResult::Action::OutputReady, code, 0, message};
}

}  // namespace aeronet::http2
