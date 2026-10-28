#include "aeronet/http2-connection.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-error-code-name.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/http2-process-result-error-msg-strings.hpp"
#include "aeronet/http2-process-result-error-msg.hpp"
#include "aeronet/http2-stream.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/metric-label.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/vector.hpp"
#include "http2-frame-type-str.hpp"
#include "http2-read-write.hpp"

#ifndef NDEBUG
#include "aeronet/string-trim.hpp"
#endif

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

// Copying a tiny body beside its frame header lets one transport write cover a batch of
// responses. Larger bodies keep their owned allocation and gather-write without a copy.
// TODO: make this configurable?
constexpr uint32_t kMaxInlineDataFrameCopySize = 256;

// Minimum number of written blocks at the front of the output queue before they are erased (see onOutputWritten()).
constexpr uint32_t kMinWrittenOutputBlocksToCompact = 16;

// Minimum number of written bytes at the front of the output buffer before they are erased while some output is still
// pending (see onOutputWritten()).
constexpr std::size_t kMinWrittenOutputBufferBytesToCompact = 16UL * 1024;

static_assert(kMaxInlineDataFrameCopySize <= kMinMaxFrameSize);

// Hands over the allocation of 'buf' (not its bytes) to an output block.
HttpMessageData OwnerOf(RawBytes&& buf) noexcept { return HttpMessageData(RawChars(std::move(buf))); }

}  // namespace

// ============================
// Queued output
// ============================

Http2Connection::OutputBlock::OutputBlock(HttpMessageData&& owner, std::span<const std::byte> payload,
                                          std::size_t bufferOffset, uint32_t maxFrameSize, uint32_t streamId,
                                          bool endStream, bool headers)
    : _owner(std::move(owner)),
      _pPayload(payload.data()),
      _payloadSize(payload.size()),
      _bufferOffset(bufferOffset),
      _maxFrameSize(maxFrameSize) {
  assert(!payload.empty());
  assert(maxFrameSize != 0);

  const uint32_t frameCount = this->frameCount();
  _remainingSize = payload.size() + (static_cast<std::size_t>(frameCount) * FrameHeader::kSize);
  if (frameCount > 1U) {
    const auto continuationHeadersSize = static_cast<uint32_t>((frameCount - 1U) * FrameHeader::kSize);
    _continuationHeaders = RawBytes32(continuationHeadersSize);
    _continuationHeaders.setSize(continuationHeadersSize);
  }

  for (uint32_t frameIndex = 0; frameIndex < frameCount; ++frameIndex) {
    const bool first = frameIndex == 0;
    const bool last = frameIndex + 1U == frameCount;
    FrameType frameType = FrameType::Data;
    uint8_t flags = FrameFlags::None;
    if (headers) {
      if (first) {
        frameType = FrameType::Headers;
        flags = ComputeHeaderFrameFlags(endStream, last);
      } else {
        frameType = FrameType::Continuation;
        flags = last ? FrameFlags::ContinuationEndHeaders : FrameFlags::None;
      }
    } else if (last && endStream) {
      flags = FrameFlags::DataEndStream;
    }

    std::byte* pHeaderData = first ? _firstHeader.data()
                                   : _continuationHeaders.data() + (static_cast<std::ptrdiff_t>(frameIndex - 1U) *
                                                                    static_cast<std::ptrdiff_t>(FrameHeader::kSize));
    WriteFrameHeader(pHeaderData, {static_cast<uint32_t>(framePayloadSize(frameIndex)), frameType, flags, streamId});
  }
}

std::size_t Http2Connection::OutputBlock::framePayloadSize(uint32_t frameIndex) const noexcept {
  const std::size_t frameOffset = static_cast<std::size_t>(frameIndex) * _maxFrameSize;
  return std::min(_payloadSize - frameOffset, static_cast<std::size_t>(_maxFrameSize));
}

std::string_view Http2Connection::OutputBlock::firstFragment() const noexcept {
  assert(!empty());
  if (_headerOffset < FrameHeader::kSize) {
    return {reinterpret_cast<const char*>(frameHeader(_frameIndex) + _headerOffset),
            FrameHeader::kSize - _headerOffset};
  }

  const std::size_t payloadSize = framePayloadSize(_frameIndex);
  assert(_framePayloadOffset < payloadSize);
  const std::size_t frameOffset = static_cast<std::size_t>(_frameIndex) * _maxFrameSize;
  return {reinterpret_cast<const char*>(_pPayload + frameOffset + _framePayloadOffset),
          payloadSize - _framePayloadOffset};
}

vector<std::string_view>::size_type Http2Connection::OutputBlock::pendingFragmentCount() const noexcept {
  assert(!empty());
  // A frame header and a payload fragment per pending frame, minus the header of the current frame once written.
  auto fragmentCount = static_cast<vector<std::string_view>::size_type>((frameCount() - _frameIndex) * 2U);
  fragmentCount -= static_cast<vector<std::string_view>::size_type>(_headerOffset == FrameHeader::kSize);
  return fragmentCount;
}

void Http2Connection::OutputBlock::appendFragments(vector<std::string_view>& fragments) const {
  assert(!empty());
  const uint32_t frameCount = this->frameCount();
  uint32_t frameIndex = _frameIndex;
  uint32_t framePayloadOffset = _framePayloadOffset;
  uint8_t headerOffset = _headerOffset;

  while (frameIndex < frameCount) {
    if (headerOffset < FrameHeader::kSize) {
      fragments.emplace_back(reinterpret_cast<const char*>(frameHeader(frameIndex) + headerOffset),
                             FrameHeader::kSize - headerOffset);
    }

    // The payload of a pending frame is never fully written (the frame would be done).
    const std::size_t payloadSize = framePayloadSize(frameIndex);
    assert(framePayloadOffset < payloadSize);
    const std::size_t frameOffset = static_cast<std::size_t>(frameIndex) * _maxFrameSize;
    fragments.emplace_back(reinterpret_cast<const char*>(_pPayload + frameOffset + framePayloadOffset),
                           payloadSize - framePayloadOffset);
    ++frameIndex;
    framePayloadOffset = 0;
    headerOffset = 0;
  }
}

void Http2Connection::OutputBlock::consume(std::size_t bytesWritten) noexcept {
  assert(bytesWritten <= _remainingSize);
  _remainingSize -= bytesWritten;

  // The written bytes never exceed the remaining frames.
  while (bytesWritten != 0) {
    if (_headerOffset < FrameHeader::kSize) {
      const std::size_t consumed = std::min<std::size_t>(bytesWritten, FrameHeader::kSize - _headerOffset);
      _headerOffset += static_cast<uint8_t>(consumed);
      bytesWritten -= consumed;
      if (bytesWritten == 0) {
        break;
      }
    }

    const std::size_t payloadRemaining = framePayloadSize(_frameIndex) - _framePayloadOffset;
    const std::size_t consumed = std::min(bytesWritten, payloadRemaining);
    _framePayloadOffset += static_cast<uint32_t>(consumed);
    bytesWritten -= consumed;
    if (_framePayloadOffset == framePayloadSize(_frameIndex)) {
      ++_frameIndex;
      _framePayloadOffset = 0;
      _headerOffset = 0;
    }
  }
}

void Http2Connection::OutputBlock::release() noexcept {
  _owner = {};
  _continuationHeaders = {};
}

// ============================
// Constructor / Destructor
// ============================

Http2Connection::Http2Connection(const Http2Config& config, bool isServer, tracing::TelemetryContext* telemetryContext)
    : _localSettings(config),
      _connectionSendWindow(static_cast<int32_t>(kDefaultInitialWindowSize)),
      _connectionRecvWindow(static_cast<int32_t>(config.connectionWindowSize)),
      // The tables of both endpoints start with the initial size (RFC 9113 §4.3.1), and headerTableSize caps both of
      // ours. A smaller encoder table needs no signaling: its entries are the newest ones of the table of the peer.
      _hpackEncoder(std::min(config.headerTableSize, kDefaultHeaderTableSize)),
      // Until the peer acknowledges our SETTINGS, its encoder may still use the initial table size: a smaller
      // headerTableSize is applied on the acknowledgement.
      _hpackDecoder(std::max(config.headerTableSize, kDefaultHeaderTableSize), config.maxHeaderListSize,
                    config.mergeUnknownRequestHeaders),
      _pTelemetryContext(telemetryContext != nullptr && telemetryContext->enabled() ? telemetryContext : nullptr),
      _isServer(isServer) {}

void Http2Connection::recordFrame(bool sent, FrameType type, uint64_t payloadBytes, uint64_t count) const noexcept {
  if (_pTelemetryContext == nullptr) {
    return;
  }

  const MetricLabel labels[]{
      {"direction", sent ? "sent" : "received"},
      {"frame.type", FrameTypeName(type)},
  };
  _pTelemetryContext->counterAdd("aeronet.http2.frames", count, labels);
  if (payloadBytes != 0) {
    _pTelemetryContext->counterAdd("aeronet.http2.frame.payload.bytes", payloadBytes, labels);
  }
}

void Http2Connection::recordHpack(bool sent, uint64_t compressedBytes, uint64_t headerListSize) const noexcept {
  if (_pTelemetryContext == nullptr) {
    return;
  }

  const MetricLabel labels[]{{"direction", sent ? "sent" : "received"}};
  _pTelemetryContext->histogram("aeronet.http2.hpack.block.compressed.bytes", static_cast<double>(compressedBytes),
                                labels);
  _pTelemetryContext->histogram("aeronet.http2.hpack.block.header_list.bytes", static_cast<double>(headerListSize),
                                labels);
  if (headerListSize != 0) {
    _pTelemetryContext->histogram("aeronet.http2.hpack.compression.ratio",
                                  static_cast<double>(compressedBytes) / static_cast<double>(headerListSize), labels);
  }
}

void Http2Connection::recordStreamOpened(bool locallyInitiated) const noexcept {
  if (_pTelemetryContext == nullptr) {
    return;
  }

  const MetricLabel labels[]{{"initiator", locallyInitiated ? "local" : "remote"}};
  _pTelemetryContext->counterAdd("aeronet.http2.streams.opened", 1UL, labels);
  _pTelemetryContext->histogram("aeronet.http2.streams.active", static_cast<double>(_activeStreamCount));
}

void Http2Connection::recordStreamClosed(uint32_t streamId, ErrorCode errorCode) const noexcept {
  if (_pTelemetryContext == nullptr) {
    return;
  }

  const bool locallyInitiated = _isServer ? (streamId & 1U) == 0 : (streamId & 1U) != 0;
  const MetricLabel labels[]{
      {"initiator", locallyInitiated ? "local" : "remote"},
      {"error.type", ErrorCodeName(errorCode)},
  };
  _pTelemetryContext->counterAdd("aeronet.http2.streams.closed", 1UL, labels);
  _pTelemetryContext->histogram("aeronet.http2.streams.active", static_cast<double>(_activeStreamCount));
}

// Returns true if walking up from `parentId` toward the root would exceed
// maxPriorityTreeDepth (or never terminates, i.e. a cycle).
bool Http2Connection::priorityDepthExceeded(uint32_t id) const noexcept {
  uint32_t depth = 0;
  while (id != 0) {
    if (++depth > _localSettings.maxPriorityTreeDepth) {
      return true;
    }
    const auto it = _streams.find(id);
    if (it == _streams.end()) {
      break;  // ancestor not tracked (idle/pruned) -> chain ends here
    }
    id = it->second.streamDependency();  // needs this accessor on Http2Stream
  }
  return false;
}

// ============================
// Connection lifecycle
// ============================

Http2Connection::ProcessResult Http2Connection::processInput(std::span<const std::byte> data,
                                                             uint32_t outputHighWaterMark) {
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
      return processFrames(data, outputHighWaterMark);

    default:
      assert(_state == ConnectionState::Closed);
      return ProcessResult{ProcessResult::Action::Closed};
  }
}

std::span<const std::byte> Http2Connection::getPendingOutput() const noexcept {
  std::size_t bufferEnd = _outputBuffer.size();
  if (_outputBlockReadPos < _outputBlocks.size()) {
    const OutputBlock& block = _outputBlocks[_outputBlockReadPos];
    if (_outputWritePos == block.bufferOffset()) {
      const std::string_view fragment = block.firstFragment();
      return {reinterpret_cast<const std::byte*>(fragment.data()), fragment.size()};
    }
    bufferEnd = block.bufferOffset();
  }
  return {_outputBuffer.data() + _outputWritePos, bufferEnd - _outputWritePos};
}

void Http2Connection::getPendingOutputFragments(vector<std::string_view>& fragments) const {
  fragments.clear();
  std::size_t bufferPos = _outputWritePos;
  vector<std::string_view>::size_type fragmentCount = 0;
  for (auto blockIndex = _outputBlockReadPos; blockIndex < _outputBlocks.size(); ++blockIndex) {
    const OutputBlock& block = _outputBlocks[blockIndex];
    fragmentCount += static_cast<vector<std::string_view>::size_type>(bufferPos < block.bufferOffset()) +
                     block.pendingFragmentCount();
    bufferPos = block.bufferOffset();
  }
  fragmentCount += static_cast<vector<std::string_view>::size_type>(bufferPos < _outputBuffer.size());
  fragments.reserve(fragmentCount);

  // The output buffer bytes before each block, the block, and the buffer bytes after the last one.
  const char* pBuffer = reinterpret_cast<const char*>(_outputBuffer.data());
  bufferPos = _outputWritePos;
  for (auto blockIndex = _outputBlockReadPos; blockIndex < _outputBlocks.size(); ++blockIndex) {
    const OutputBlock& block = _outputBlocks[blockIndex];
    if (bufferPos < block.bufferOffset()) {
      fragments.emplace_back(pBuffer + bufferPos, block.bufferOffset() - bufferPos);
      bufferPos = block.bufferOffset();
    }
    block.appendFragments(fragments);
  }
  if (bufferPos < _outputBuffer.size()) {
    fragments.emplace_back(pBuffer + bufferPos, _outputBuffer.size() - bufferPos);
  }
  assert(fragments.size() == fragmentCount);
}

void Http2Connection::onOutputWritten(std::size_t bytesWritten) {
  assert(bytesWritten <= pendingOutputSize());

  while (bytesWritten != 0 && _outputBlockReadPos < _outputBlocks.size()) {
    OutputBlock& block = _outputBlocks[_outputBlockReadPos];
    // The output buffer bytes before the block are sent first.
    const std::size_t bufferBytes = std::min(bytesWritten, block.bufferOffset() - _outputWritePos);
    _outputWritePos += bufferBytes;
    bytesWritten -= bufferBytes;
    if (bytesWritten == 0) {
      break;
    }

    const std::size_t consumed = std::min(bytesWritten, block.remainingSize());
    block.consume(consumed);
    _outputBlocksSize -= consumed;
    bytesWritten -= consumed;
    if (block.empty()) {
      block.release();
      ++_outputBlockReadPos;
    }
  }
  // The remaining bytes follow the last block.
  _outputWritePos += bytesWritten;
  assert(_outputWritePos <= _outputBuffer.size());

  if (_outputBlockReadPos == _outputBlocks.size()) {
    _outputBlocks.clear();
    _outputBlockReadPos = 0;
  } else if (_outputBlockReadPos >= kMinWrittenOutputBlocksToCompact &&
             2U * _outputBlockReadPos >= _outputBlocks.size()) {
    // A peer that keeps the queue from draining completely would otherwise make it grow forever with written (already
    // released) blocks. Erasing them once they are at least half of the queue moves at most as many blocks as written.
    _outputBlocks.erase(_outputBlocks.begin(),
                        _outputBlocks.begin() + static_cast<std::ptrdiff_t>(_outputBlockReadPos));
    _outputBlockReadPos = 0;
  }

  // The output buffer is kept, with its capacity: erase its written bytes once all are written (without any move), or
  // once they are at least half of it (moving at most as many bytes as written), so that it does not grow forever
  // either. The pending blocks are all sent after them.
  if (_outputWritePos != 0 &&
      (_outputWritePos == _outputBuffer.size() ||
       (_outputWritePos >= kMinWrittenOutputBufferBytesToCompact && 2U * _outputWritePos >= _outputBuffer.size()))) {
    for (auto blockIndex = _outputBlockReadPos; blockIndex < _outputBlocks.size(); ++blockIndex) {
      _outputBlocks[blockIndex].onBufferFrontErased(_outputWritePos);
    }
    _outputBuffer.erase_front(_outputWritePos);
    _outputWritePos = 0;
  }
}

void Http2Connection::discardPendingOutput() noexcept {
  _outputBlocks.clear();
  _outputBlocksSize = 0;
  _outputBlockReadPos = 0;
  _outputBuffer.clear();
  _outputWritePos = 0;
}

void Http2Connection::queueOutputBlock(HttpMessageData&& owner, std::span<const std::byte> payload, uint32_t streamId,
                                       bool endStream, bool headers) {
  const OutputBlock& block = _outputBlocks.emplace_back(std::move(owner), payload, _outputBuffer.size(),
                                                        _peerSettings.maxFrameSize, streamId, endStream, headers);
  _outputBlocksSize += block.remainingSize();
}

void Http2Connection::initiateGoAway(ErrorCode errorCode, ErrorMsg msg) {
  if (_state == ConnectionState::Closed || _state == ConnectionState::GoAwaySent) {
    return;
  }

  WriteGoAwayFrame(_outputBuffer, _lastPeerStreamId, errorCode, msg);
  recordFrame(true, FrameType::GoAway, 8U + ConvertProcessResultErrorMsgToSv(msg).size());
  _state = ConnectionState::GoAwaySent;
}

Http2Connection::ProcessResult Http2Connection::checkTimeouts(std::chrono::steady_clock::time_point now,
                                                              std::chrono::steady_clock::time_point lastActivity) {
  using TimePoint = std::chrono::steady_clock::time_point;
  if (_settingsAckDeadline != TimePoint{} && now >= _settingsAckDeadline) {
    return connectionError(ErrorCode::SettingsTimeout, ErrorMsg::SETTINGSTimeout);
  }
  if (_pingAckDeadline != TimePoint{}) {
    if (now >= _pingAckDeadline) {
      return connectionError(ErrorCode::NoError, ErrorMsg::PINGTimeout);
    }
  } else if (_localSettings.pingInterval.count() > 0 && _activeStreamCount != 0 &&
             now - lastActivity >= _localSettings.pingInterval) {
    sendPing(PingFrame{});
    _pingAckDeadline = now + _localSettings.pingTimeout;
    return ProcessResult{ProcessResult::Action::OutputReady};
  }
  return ProcessResult{ProcessResult::Action::Continue};
}

void Http2Connection::sendPing(PingFrame pingFrame) {
  WritePingFrame(_outputBuffer, pingFrame);
  recordFrame(true, FrameType::Ping, sizeof(pingFrame.opaqueData));
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

  const auto streamId = it->first;

  log::debug("Stream {} is now closed with error code {}", streamId, static_cast<uint32_t>(errorCode));
  if (errorCode != ErrorCode::NoError) {
    it->second.setErrorCode(errorCode);
  }
  assert(_activeStreamCount != 0);
  --_activeStreamCount;
  recordStreamClosed(streamId, errorCode);

  if (_pSink != nullptr) {
    _pSink->onStreamClosed(streamId);
  }

  // Don't remove immediately - keep for a short time for late frames.
  _closedStreamsFifo.push_back(streamId);
  pruneClosedStreams();
}

void Http2Connection::pruneClosedStreams() {
  while (_closedStreamsFifo.size() > kClosedStreamsMaxRetained) {
    const auto streamId = _closedStreamsFifo.front();
    _closedStreamsFifo.pop_front();

    const auto it = _streams.find(streamId);
    assert(it != _streams.end() && it->second.isClosed());
    _streams.erase(it);
    _maxPrunedStreamId = std::max(_maxPrunedStreamId, streamId);
  }
}

// ============================
// Frame sending
// ============================

ErrorCode Http2Connection::prepareSendHeaders(uint32_t streamId, bool endStream) {
  auto [it, inserted] =
      _streams.try_emplace(streamId, streamId, _peerSettings.initialWindowSize, _localSettings.initialWindowSize);
  if (inserted) {
    // Created new stream
    if (!canCreateStreams()) {
      _streams.erase(it);
      return ErrorCode::RefusedStream;
    }

    ++_activeStreamCount;
    recordStreamOpened(true);
    if (!isPeerStream(streamId)) {
      _lastLocalStreamId = std::max(_lastLocalStreamId, streamId);
    }
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

  uint64_t headerListSize = 0;
  if (!target.empty()) {
    // pseudo headers for requests.
    const std::string_view methodStr = http::MethodToStr(method);
    const std::string_view scheme = isTlsRequest ? "https" : "http";

    headerListSize += _hpackEncoder.encode(_outputBuffer, http::PseudoHeaderMethod, methodStr);
    headerListSize += _hpackEncoder.encode(_outputBuffer, http::PseudoHeaderScheme, scheme);
    headerListSize += _hpackEncoder.encode(_outputBuffer, http::PseudoHeaderAuthority, authority);
    headerListSize += _hpackEncoder.encode(_outputBuffer, http::PseudoHeaderPath, target);
  }

  // Encode headers
  encodeHeaders(streamId, target.empty() ? http::StatusCode{} : http::MagicForHttpRequest, headersView, endStream,
                oldSize, pGlobalHeaders, headerListSize);

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
  encodeHeaders(streamId, statusCode, headersView, endStream, oldSize, pGlobalHeaders, 0);

  return err;
}

ErrorCode Http2Connection::prepareSendData(uint32_t streamId, std::size_t dataSize, bool endStream) {
  Http2Stream* pStream = getStream(streamId);
  if (pStream == nullptr) [[unlikely]] {
    return ErrorCode::ProtocolError;
  }
  if (!pStream->canSend()) {
    return ErrorCode::StreamClosed;
  }
  if (std::cmp_less(_connectionSendWindow, dataSize)) {
    return ErrorCode::FlowControlError;
  }
  if (!pStream->consumeSendWindow(dataSize)) {
    return ErrorCode::FlowControlError;
  }

  _connectionSendWindow -= static_cast<int32_t>(dataSize);

  // canSend() above ensures the stream is Open or HalfClosedRemote, which are exactly the states onSendData() handles.
  const ErrorCode err = pStream->onSendData(endStream);
  assert(err == ErrorCode::NoError);

  recordFrame(true, FrameType::Data, dataSize,
              (dataSize + _peerSettings.maxFrameSize - 1U) / _peerSettings.maxFrameSize);

  return err;
}

ErrorCode Http2Connection::sendData(uint32_t streamId, std::span<const std::byte> data, bool endStream) {
  const ErrorCode err = prepareSendData(streamId, data.size(), endStream);
  if (err != ErrorCode::NoError) {
    return err;
  }

  if (data.size() <= kMaxInlineDataFrameCopySize) {
    std::byte* pData =
        PrepareDataFrameGetStartPtr(_outputBuffer, streamId, static_cast<uint32_t>(data.size()), endStream);
    Copy(data.data(), data.size(), pData);
  } else {
    RawBytes copy(data);
    const std::span<const std::byte> copiedData(copy.data(), copy.size());
    queueDataBlock(OwnerOf(std::move(copy)), copiedData, streamId, endStream);
  }
  return ErrorCode::NoError;
}

ErrorCode Http2Connection::sendData(uint32_t streamId, HttpMessageData&& owner, std::span<const std::byte> data,
                                    bool endStream) {
  const ErrorCode err = prepareSendData(streamId, data.size(), endStream);
  if (err != ErrorCode::NoError) {
    return err;
  }

  assert(!data.empty());
  if (data.size() <= kMaxInlineDataFrameCopySize) {
    std::byte* pData =
        PrepareDataFrameGetStartPtr(_outputBuffer, streamId, static_cast<uint32_t>(data.size()), endStream);
    Copy(data.data(), data.size(), pData);
  } else {
    queueDataBlock(std::move(owner), data, streamId, endStream);
  }
  return ErrorCode::NoError;
}

ErrorCode Http2Connection::sendData(uint32_t streamId, RawBytes&& owner, std::size_t dataOffset, std::size_t dataSize,
                                    bool endStream) {
  assert(dataOffset <= owner.size());
  assert(dataSize <= owner.size() - dataOffset);
  const std::span<const std::byte> data(owner.data() + dataOffset, dataSize);
  return sendData(streamId, OwnerOf(std::move(owner)), data, endStream);
}

[[nodiscard]] std::pair<std::byte*, ErrorCode> Http2Connection::sendDataInline(uint32_t streamId, std::size_t dataSize,
                                                                               bool endStream) {
  const ErrorCode err = prepareSendData(streamId, dataSize, endStream);
  if (err != ErrorCode::NoError) {
    return {nullptr, err};
  }

  return {PrepareDataFrameGetStartPtr(_outputBuffer, streamId, static_cast<uint32_t>(dataSize), endStream),
          ErrorCode::NoError};
}

void Http2Connection::sendRstStream(uint32_t streamId, ErrorCode errorCode) {
  WriteRstStreamFrame(_outputBuffer, streamId, errorCode);

  recordFrame(true, FrameType::RstStream, sizeof(uint32_t));

  const auto it = _streams.find(streamId);
  if (it != _streams.end()) {
    it->second.onSendRstStream();
    it->second.setErrorCode(errorCode);
    closeStream(it, errorCode);
    if (_pSink != nullptr) {
      _pSink->onStreamReset(streamId, errorCode);
    }
  }
}

void Http2Connection::holdStreamRecvWindow(uint32_t streamId) noexcept {
  Http2Stream* pStream = getStream(streamId);
  if (pStream != nullptr) {
    pStream->holdRecvWindow(true);
  }
}

void Http2Connection::releaseStreamRecvWindow(uint32_t streamId) {
  Http2Stream* pStream = getStream(streamId);
  if (pStream == nullptr || !pStream->recvWindowHeld()) {
    return;
  }
  pStream->holdRecvWindow(false);
  // Same threshold as the replenishment on reception (handleDataFrame()): above it, the next DATA frame replenishes it.
  const int32_t initialStreamWindow = static_cast<int32_t>(_localSettings.initialWindowSize);
  if (pStream->canReceive() && pStream->recvWindow() <= initialStreamWindow / 2) {
    sendWindowUpdate(streamId, static_cast<uint32_t>(initialStreamWindow - pStream->recvWindow()));
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
  recordFrame(true, FrameType::WindowUpdate, sizeof(uint32_t));

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

Http2Connection::ProcessResult Http2Connection::processFrames(std::span<const std::byte> data,
                                                              uint32_t outputHighWaterMark) {
  std::size_t totalConsumed = 0;

  while (data.size() >= FrameHeader::kSize) {
    if (pendingOutputSize() >= outputHighWaterMark) {
      break;
    }
    const FrameHeader header = ParseFrameHeader(data);
    const uint32_t headerLength = header.length();

    // Check frame size limits
    if (headerLength > _localSettings.maxFrameSize) {
      return connectionError(ErrorCode::FrameSizeError, ErrorMsg::FrameExceedsMaximumSize);
    }

    const uint32_t totalFrameSize = FrameHeader::kSize + headerLength;
    if (data.size() < totalFrameSize) {
      // Need more data
      break;
    }

    const auto payload = data.subspan(FrameHeader::kSize, headerLength);

    recordFrame(false, header.type(), headerLength);

    ProcessResult result = processFrame(header, payload);

    // The current frame was processed, include it in bytes consumed.
    totalConsumed += totalFrameSize;

    if (result.action != ProcessResult::Action::Continue && result.action != ProcessResult::Action::OutputReady) {
      result.bytesConsumed = totalConsumed;
      return result;
    }

    data = data.subspan(totalFrameSize);
  }

  return {hasPendingOutput() ? ProcessResult::Action::OutputReady : ProcessResult::Action::Continue, totalConsumed};
}

Http2Connection::ProcessResult Http2Connection::processFrame(FrameHeader header, std::span<const std::byte> payload) {
  // CONTINUATION frames must follow HEADERS/PUSH_PROMISE
  if (_expectingContinuation && header.type() != FrameType::Continuation) {
    return connectionError(ErrorCode::ProtocolError, ErrorMsg::ExpectedCONTINUATIONFrame);
  }

  switch (header.type()) {
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
      // A server cannot receive PUSH_PROMISE (RFC 9113 §8.4), and aeronet clients disable push with
      // SETTINGS_ENABLE_PUSH = 0, making it a connection error for them as well (RFC 9113 §6.6).
      return connectionError(ErrorCode::ProtocolError, ErrorMsg::UnexpectedPUSH_PROMISE);
    case FrameType::Ping:
      return handlePingFrame(header, payload);
    case FrameType::GoAway:
      if (header.streamId() != 0) {
        return connectionError(ErrorCode::ProtocolError, ErrorMsg::GOAWAYFrameOnNonZeroStream);
      }
      return handleGoAwayFrame(payload);
    case FrameType::WindowUpdate:
      return handleWindowUpdateFrame(header, payload);
    case FrameType::Continuation:
      return handleContinuationFrame(header, payload);
    default:
      // Unknown frame types are ignored (RFC 9113 §4.1)
      log::warn("Ignoring unknown frame type {}", static_cast<uint32_t>(header.type()));
      return ProcessResult{ProcessResult::Action::Continue};
  }
}

Http2Connection::ProcessResult Http2Connection::handleDataFrame(FrameHeader header,
                                                                std::span<const std::byte> payload) {
  const uint32_t streamId = header.streamId();
  if (streamId == 0) {
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
  int32_t payloadSize = static_cast<int32_t>(payload.size());

  if (payloadSize > _connectionRecvWindow) {
    return connectionError(ErrorCode::FlowControlError, ErrorMsg::ConnectionFlowControlExceeded);
  }
  _connectionRecvWindow -= payloadSize;

  auto it = _streams.find(streamId);
  if (it == _streams.end()) {
    // RFC 9113 §5.1: DATA on an idle stream is a connection error.
    if (isIdleStream(streamId)) [[unlikely]] {
      return connectionError(ErrorCode::ProtocolError, ErrorMsg::DATAOnIdleStream);
    }
    // Stream may have been reset. The discarded frame still consumed connection-level credit.
    replenishConnectionRecvWindow();
    return ProcessResult{ProcessResult::Action::Continue};
  }

  if (!it->second.canReceive()) {
    replenishConnectionRecvWindow();
    if (it->second.resetSent()) {
      // Sent before the peer received our RST_STREAM: ignored (RFC 9113 §5.1).
      return ProcessResult{ProcessResult::Action::Continue};
    }
    return streamError(streamId, ErrorCode::StreamClosed, ErrorMsg::DATAOnClosedStream);
  }

  if (!it->second.consumeRecvWindow(static_cast<uint32_t>(payloadSize))) {
    return streamError(streamId, ErrorCode::FlowControlError, ErrorMsg::StreamFlowControlExceeded);
  }

  const bool endStream = header.hasFlag(FrameFlags::DataEndStream);

  // canReceive() above already ensures the stream is Open or HalfClosedLocal,
  // which are exactly the states onRecvData() handles — so this cannot fail.
  [[maybe_unused]] ErrorCode err = it->second.onRecvData(endStream);
  assert(err == ErrorCode::NoError);

  // Invoke callback
  if (_pSink != nullptr) {
    _pSink->onData(streamId, frame.data, endStream);

    // The callback may have reset streams, whose pruning can move the map entries. This stream, closed or not, is not
    // pruned yet: the closed streams are retained a while for late frames.
    it = _streams.find(streamId);
    assert(it != _streams.end());

    // Replenish a receive window only after half its configured credit has been consumed. Restoring the
    // full credit at once keeps large transfers moving while avoiding two tiny WINDOW_UPDATE frames for
    // every DATA frame. A stream-level update is useless after END_STREAM, or for a stream whose window is held or
    // that the callback reset, but connection-level credit is retained for later streams on the same connection.
    if (payloadSize > 0) {
      Http2Stream& stream = it->second;
      const int32_t initialStreamWindow = static_cast<int32_t>(_localSettings.initialWindowSize);
      if (!endStream && !stream.recvWindowHeld() && stream.canReceive() &&
          stream.recvWindow() <= initialStreamWindow / 2) {
        sendWindowUpdate(streamId, static_cast<uint32_t>(initialStreamWindow - stream.recvWindow()));
      }

      replenishConnectionRecvWindow();
    }
  }

  if (endStream && it->second.isClosed()) {
    closeStream(it);
  }

  return ProcessResult{ProcessResult::Action::Continue};
}

void Http2Connection::replenishConnectionRecvWindow() {
  const int32_t initialConnectionWindow = static_cast<int32_t>(_localSettings.connectionWindowSize);
  if (_connectionRecvWindow <= initialConnectionWindow / 2) {
    sendWindowUpdate(0, static_cast<uint32_t>(initialConnectionWindow - _connectionRecvWindow));
  }
}

Http2Connection::ProcessResult Http2Connection::handleHeadersFrame(FrameHeader header,
                                                                   std::span<const std::byte> payload) {
  const uint32_t streamId = header.streamId();
  if (streamId == 0) {
    return connectionError(ErrorCode::ProtocolError, ErrorMsg::HEADERSFrameOnStreamZero);
  }

  HeadersFrame frame;
  uint32_t streamDependency;
  FrameParseResult parseResult = ParseHeadersFrame(header, payload, frame, streamDependency);
  if (parseResult != FrameParseResult::Ok) {
    if (parseResult == FrameParseResult::InvalidPadding) {
      return connectionError(ErrorCode::ProtocolError, ErrorMsg::InvalidPaddingInHEADERSFrame);
    }
    if (parseResult == FrameParseResult::InvalidDependency) {
      return streamError(streamId, ErrorCode::ProtocolError, ErrorMsg::StreamDependsOnItself);
    }
    return connectionError(ErrorCode::FrameSizeError, ErrorMsg::InvalidHEADERSFrame);
  }

  // Get or create stream
  auto [it, inserted] =
      _streams.try_emplace(streamId, streamId, _peerSettings.initialWindowSize, _localSettings.initialWindowSize);
  if (inserted) {
    // Only a client opens streams with HEADERS (a server would push them, which is disabled), with odd and increasing
    // ids, and not after the GOAWAY of this endpoint.
    if (!_isServer || !isPeerStream(streamId) || streamId <= _lastPeerStreamId || _state == ConnectionState::GoAwaySent)
        [[unlikely]] {
      _streams.erase(it);
      return handleHeadersOnUnknownStream(header, frame);
    }

    ++_activeStreamCount;
    recordStreamOpened(false);
    _lastPeerStreamId = streamId;

    // The last stream of the budget of the connection (Http2Config::maxStreamsPerConnection): a graceful GOAWAY makes
    // the client open its next streams on another connection, this one closing once its streams are served (see
    // goAwayCompleted()). The stream ids of a client are odd: this one is its (streamId / 2 + 1)th.
    if ((streamId / 2U) + 1U >= _localSettings.maxStreamsPerConnection) [[unlikely]] {
      initiateGoAway(ErrorCode::NoError, ErrorMsg::MaxStreamsPerConnectionReached);
    }

    // For peer-created streams, enforce OUR max concurrent streams limit (not _peerSettings which limits streams WE
    // initiate). Exceeding it is a stream error (RFC 9113 §5.1.2) that a peer may hit before receiving our SETTINGS:
    // REFUSED_STREAM tells it that the request can be retried. The reset stream still decodes its field block below,
    // which keeps the HPACK dynamic table in sync.
    if (_activeStreamCount > _localSettings.maxConcurrentStreams) [[unlikely]] {
      sendRstStream(streamId, ErrorCode::RefusedStream);
      // Its pruning of the closed streams can move the map entries.
      it = _streams.find(streamId);
      assert(it != _streams.end());
    }
  }

  // Handle priority if present
  if (header.hasFlag(FrameFlags::HeadersPriority)) {
    const uint32_t effectiveDependency = priorityDepthExceeded(streamDependency) ? 0U : streamDependency;
    it->second.setPriority(effectiveDependency, frame.weightMinusOne, frame.exclusive);
  }

  const std::span<const std::byte> headerBlockFragment = frame.headerBlockFragment();
  const bool endStream = header.hasFlag(FrameFlags::HeadersEndStream);
  if (!header.hasFlag(FrameFlags::HeadersEndHeaders)) {
    return startFieldBlock(streamId, headerBlockFragment, endStream);
  }
  return processFieldBlock(it, headerBlockFragment, endStream);
}

Http2Connection::ProcessResult Http2Connection::handleHeadersOnUnknownStream(FrameHeader header,
                                                                             const HeadersFrame& frame) {
  const uint32_t streamId = header.streamId();
  // A new stream of the client after the GOAWAY of this endpoint, whose last stream id is _lastPeerStreamId, is
  // ignored (RFC 9113 §6.8), like the frames on a stream closed then forgotten (§5.1). Their field block is decoded all
  // the same: it updates the HPACK dynamic table of the connection, which the next field blocks refer to (§4.3).
  const bool newStreamAfterGoAway = _isServer && isPeerStream(streamId) && streamId > _lastPeerStreamId;
  if (!newStreamAfterGoAway && !wasPruned(streamId)) {
    // An unexpected stream identifier (RFC 9113 §5.1.1): a stream initiated by the server, or a lower one than the last
    // stream opened by the client (skipped by it), or a stream a client did not open.
    if (!_isServer) {
      return connectionError(ErrorCode::ProtocolError, ErrorMsg::HEADERSOnIdleStream);
    }
    return connectionError(ErrorCode::ProtocolError, isPeerStream(streamId)
                                                         ? ErrorMsg::StreamIdNotIncreasing
                                                         : ErrorMsg::ServerInitiatedStreamIdFromClient);
  }

  const std::span<const std::byte> headerBlockFragment = frame.headerBlockFragment();
  if (!header.hasFlag(FrameFlags::HeadersEndHeaders)) {
    return startFieldBlock(streamId, headerBlockFragment, header.hasFlag(FrameFlags::HeadersEndStream));
  }
  return discardFieldBlock(headerBlockFragment);
}

Http2Connection::ProcessResult Http2Connection::startFieldBlock(uint32_t streamId, std::span<const std::byte> fragment,
                                                                bool endStream) {
  // Security hardening (CVE-2024-27316): reject oversized initial header block fragment
  // to prevent unbounded memory growth via CONTINUATION flood.
  if (fragment.size() > kMaxHeaderBlockAccumulationSize) [[unlikely]] {
    return connectionError(ErrorCode::EnhanceYourCalm, ErrorMsg::HeaderBlockTooLarge);
  }
  _expectingContinuation = true;
  _headerBlockStreamId = streamId;
  _headerBlockEndStream = endStream;
  _headerBlockBuffer.assign(fragment);
  return ProcessResult{ProcessResult::Action::Continue};
}

Http2Connection::ProcessResult Http2Connection::handlePriorityFrame(FrameHeader header,
                                                                    std::span<const std::byte> payload) {
  const uint32_t streamId = header.streamId();
  if (streamId == 0) {
    return connectionError(ErrorCode::ProtocolError, ErrorMsg::PRIORITYFrameOnStreamZero);
  }
  if (payload.size() != sizeof(uint32_t) + 1U) {
    return connectionError(ErrorCode::FrameSizeError, ErrorMsg::InvalidPRIORITYFrame);
  }

  PriorityFrame frame = ParsePriorityFrame(payload);

  if (frame.streamDependency == streamId) {
    return streamError(streamId, ErrorCode::ProtocolError, ErrorMsg::StreamDependsOnItself);
  }

  Http2Stream* pStream = getStream(streamId);
  if (pStream != nullptr) {
    const uint32_t effectiveDependency = priorityDepthExceeded(frame.streamDependency) ? 0U : frame.streamDependency;
    pStream->setPriority(effectiveDependency, frame.weightMinusOne, frame.exclusive);
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
  const uint32_t streamId = header.streamId();
  if (streamId == 0) {
    return connectionError(ErrorCode::ProtocolError, ErrorMsg::RST_STREAMFrameOnStreamZero);
  }

  RstStreamFrame frame;
  FrameParseResult parseResult = ParseRstStreamFrame(payload, frame);
  if (parseResult != FrameParseResult::Ok) [[unlikely]] {
    return connectionError(ErrorCode::FrameSizeError, ErrorMsg::InvalidRST_STREAMFrame);
  }

  auto it = _streams.find(streamId);
  if (it == _streams.end()) {
    // RFC 9113 §6.4: a RST_STREAM on an idle stream is a connection error. On a closed stream, it is ignored.
    if (isIdleStream(streamId)) [[unlikely]] {
      return connectionError(ErrorCode::ProtocolError, ErrorMsg::RST_STREAMOnIdleStream);
    }
    return ProcessResult{ProcessResult::Action::Continue};
  }
  // A stream this endpoint reset is already closed: the peer's own RST_STREAM is ignored (RFC 9113 §5.1).
  if (!it->second.resetSent()) {
    it->second.onRecvRstStream();
    it->second.setErrorCode(frame.errorCode);
    closeStream(it, frame.errorCode);
    if (_pSink != nullptr) {
      _pSink->onStreamReset(streamId, frame.errorCode);
    }
  }

  return ProcessResult{ProcessResult::Action::Continue};
}

Http2Connection::ProcessResult Http2Connection::handleSettingsFrame(FrameHeader header,
                                                                    std::span<const std::byte> payload) {
  if (header.streamId() != 0) {
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
    _hpackDecoder.setMaxDynamicTableSize(_localSettings.headerTableSize);
    _settingsAckDeadline = {};
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
        // Capped by our own limit: a peer advertising 4 GiB would make the encoder keep every indexed field it sends.
        _hpackEncoder.setMaxDynamicTableSize(std::min(value, _localSettings.headerTableSize));
        break;
      case SettingsParameter::EnablePush:
        // RFC 9113 §6.5.2: the value MUST be 0 or 1, and a server MUST NOT send 1. As aeronet never pushes,
        // the value sent by a client is not needed.
        if (value > 1U || (value == 1U && !_isServer)) {
          return connectionError(ErrorCode::ProtocolError, ErrorMsg::InvalidENABLE_PUSHValue);
        }
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
        log::debug("Ignoring unknown HTTP/2 SETTINGS parameter id={} (0x{:04x}), value={}", static_cast<uint16_t>(id),
                   static_cast<uint16_t>(id), value);
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
  if (header.streamId() != 0) [[unlikely]] {
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
    recordFrame(true, FrameType::Ping, sizeof(frame.opaqueData));
    return ProcessResult{ProcessResult::Action::OutputReady};
  }

  // The peer is alive (see checkTimeouts()).
  _pingAckDeadline = {};
  return ProcessResult{ProcessResult::Action::Continue};
}

Http2Connection::ProcessResult Http2Connection::handleGoAwayFrame(std::span<const std::byte> payload) {
  GoAwayFrame frame;
  FrameParseResult parseResult = ParseGoAwayFrame(payload, frame);
  if (parseResult != FrameParseResult::Ok) [[unlikely]] {
    return connectionError(ErrorCode::FrameSizeError, ErrorMsg::InvalidGOAWAYFrame);
  }

  // The GOAWAY of this endpoint, if it sent one first, still applies to the new streams of the peer (see
  // handleHeadersFrame()).
  if (_state != ConnectionState::GoAwaySent) {
    _state = ConnectionState::GoAwayReceived;
  }

  if (_pSink != nullptr) {
    std::string_view debugData(reinterpret_cast<const char*>(frame.debugData.data()), frame.debugData.size());
    _pSink->onGoAway(frame.lastStreamId, frame.errorCode, debugData);
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

  const uint32_t streamId = header.streamId();
  if (frame.windowSizeIncrement == 0) {
    if (streamId == 0) {
      return connectionError(ErrorCode::ProtocolError, ErrorMsg::ZeroWINDOW_UPDATEIncrementOnConnection);
    }
    return streamError(streamId, ErrorCode::ProtocolError, ErrorMsg::ZeroWINDOW_UPDATEIncrement);
  }

  if (streamId == 0) {
    // Connection-level
    const int64_t newWindow = static_cast<int64_t>(_connectionSendWindow) + frame.windowSizeIncrement;
    if (std::cmp_greater(newWindow, kMaxWindowSize)) {
      return connectionError(ErrorCode::FlowControlError, ErrorMsg::ConnectionWindowOverflow);
    }
    _connectionSendWindow = static_cast<int32_t>(newWindow);
  } else {
    // Stream-level
    Http2Stream* pStream = getStream(streamId);
    if (pStream != nullptr) {
      ErrorCode err = pStream->increaseSendWindow(frame.windowSizeIncrement);
      if (err != ErrorCode::NoError) {
        return streamError(streamId, err, ErrorMsg::StreamWindowOverflow);
      }
    } else if (isIdleStream(streamId)) [[unlikely]] {
      // RFC 9113 §5.1: WINDOW_UPDATE on an idle stream is a connection error. On a closed stream, it is ignored.
      return connectionError(ErrorCode::ProtocolError, ErrorMsg::WINDOW_UPDATEOnIdleStream);
    }
  }

  if (_pSink != nullptr) {
    _pSink->onWindowUpdate(streamId, frame.windowSizeIncrement);
  }

  return ProcessResult{ProcessResult::Action::Continue};
}

Http2Connection::ProcessResult Http2Connection::handleContinuationFrame(FrameHeader header,
                                                                        std::span<const std::byte> payload) {
  if (!_expectingContinuation) [[unlikely]] {
    return connectionError(ErrorCode::ProtocolError, ErrorMsg::UnexpectedCONTINUATIONFrame);
  }

  if (header.streamId() != _headerBlockStreamId) [[unlikely]] {
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

  // An unknown stream when its HEADERS frame was ignored (see handleHeadersOnUnknownStream()), or when it was reset and
  // pruned meanwhile: its field block is only decoded.
  const auto it = _streams.find(_headerBlockStreamId);
  const std::span<const std::byte> headerBlock(_headerBlockBuffer);
  const ProcessResult res =
      it == _streams.end() ? discardFieldBlock(headerBlock) : processFieldBlock(it, headerBlock, _headerBlockEndStream);
  _headerBlockBuffer.clear();
  _headerBlockStreamId = 0;
  return res;
}

// ============================
// HPACK
// ============================

void Http2Connection::encodeHeaders(uint32_t streamId, http::StatusCode statusCode, HeadersView headersView,
                                    bool endStream, std::size_t oldSize, const ConcatenatedHeaders* pGlobalHeaders,
                                    uint64_t headerListSize) {
  assert(statusCode == 0 || statusCode == http::MagicForHttpRequest || (statusCode >= 100 && statusCode <= 999));

  auto headersFirst = headersView.begin();
  auto headersLast = headersView.end();

  if (statusCode == http::MagicForHttpRequest) {
    // For requests, skip the Host header (we use :authority instead)
    assert(headersFirst != headersLast && (*headersFirst).name == http::Host);
    ++headersFirst;
  } else if (statusCode >= 100) {
    // Encode :status pseudo-header first if present
    char statusBuf[3];
    writeStatusCode(statusBuf, statusCode);
    const std::string_view statusStr(statusBuf, sizeof(statusBuf));
    headerListSize += _hpackEncoder.encode(_outputBuffer, http::PseudoHeaderStatus, statusStr);
  }

  for (; headersFirst != headersLast; ++headersFirst) {
    const auto [name, value] = *headersFirst;

    // All header names should be normalized to lower-case at this step.
    assert(std::ranges::all_of(name, [](char ch) { return ch < 'A' || ch > 'Z'; }));

    // RFC 9113 §8.2.1: an HTTP/2 field value must not carry leading/trailing OWS (unlike HTTP/1.1, where it is
    // tolerated). HttpMessage / HttpResponse guarantees that values are always trimmed.
    assert(value == TrimOws(value));

    headerListSize += _hpackEncoder.encode(_outputBuffer, name, value, HpackEncoder::IndexingModeOf(name, value));
  }

  if (pGlobalHeaders != nullptr) {
    const std::string_view concatenatedGlobalHeaders = pGlobalHeaders->fullStringWithLastSep();
    for (const auto [headerName, headerValue] : HeadersView(concatenatedGlobalHeaders)) {
      // Global header names should have been validated in the config.
      assert(std::ranges::all_of(headerName, [](char ch) { return ch < 'A' || ch > 'Z'; }));
      // Skip if already present in request-specific headers. We can use case sensitive search because global headers
      // are already lower-cased in the config, and request-specific headers are lower-cased above.
      std::string_view headerNameWithColon(headerName.data(), headerName.size() + http::HeaderSep.size());
      if (headersView.containsCaseSensitive(headerNameWithColon)) {
        // Skip if already present in request-specific headers
        continue;
      }

      // The same in each response: always indexed.
      assert(headerValue == TrimOws(headerValue));
      headerListSize += _hpackEncoder.encode(_outputBuffer, headerName, headerValue);
    }
  }

  const uint32_t headerBlockSize = static_cast<uint32_t>(_outputBuffer.size() - oldSize);
  const auto outputSizeBeforeHeaders = oldSize - FrameHeader::kSize;
  recordHpack(true, headerBlockSize, headerListSize);

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
    recordFrame(true, FrameType::Headers, headerBlockSize);
    return;
  }
  const uint64_t continuationBytes = headerBlockSize - _peerSettings.maxFrameSize;
  if (outputSizeBeforeHeaders == 0) {
    // Transfer the allocation intact and interleave frame headers with views over the
    // original HPACK bytes.
    const std::span<const std::byte> headerBlock(_outputBuffer.data() + oldSize, headerBlockSize);
    HttpMessageData owner = OwnerOf(std::move(_outputBuffer));
    _outputBuffer = {};
    _outputWritePos = 0;
    queueOutputBlock(std::move(owner), headerBlock, streamId, endStream, true);
  } else {
    // Keep the earlier frames of the output buffer ahead of this oversized header block, which is copied out of it (the
    // buffer may move as it grows). Oversized fields are rare.
    RawBytes owner(std::span<const std::byte>(_outputBuffer.data() + oldSize, headerBlockSize));
    _outputBuffer.setSize(outputSizeBeforeHeaders);
    const std::span<const std::byte> headerBlock(owner.data(), owner.size());
    queueOutputBlock(OwnerOf(std::move(owner)), headerBlock, streamId, endStream, true);
  }
  recordFrame(true, FrameType::Headers, _peerSettings.maxFrameSize);
  recordFrame(true, FrameType::Continuation, continuationBytes,
              (continuationBytes + _peerSettings.maxFrameSize - 1U) / _peerSettings.maxFrameSize);
}

Http2Connection::ProcessResult Http2Connection::discardFieldBlock(std::span<const std::byte> headerBlock) {
  const auto decodeResult = _hpackDecoder.decode(headerBlock);
  if (decodeResult.isCompressionError()) {
    return connectionError(ErrorCode::CompressionError, ErrorMsg::HPACKDecodingFailed);
  }
  recordHpack(false, headerBlock.size(), decodeResult.headerListSize);
  return ProcessResult{ProcessResult::Action::Continue};
}

Http2Connection::ProcessResult Http2Connection::processFieldBlock(StreamsMap::iterator it,
                                                                  std::span<const std::byte> headerBlock,
                                                                  bool endStream) {
  if (it->second.resetSent()) {
    return discardFieldBlock(headerBlock);
  }

  const uint32_t streamId = it->first;
  const ErrorCode err = it->second.onRecvHeaders(endStream);
  if (err != ErrorCode::NoError) [[unlikely]] {
    // A field block the stream cannot accept is decoded all the same: the peer encoded it with the HPACK dynamic table
    // of the connection, which its next field blocks refer to (RFC 9113 §4.3).
    ProcessResult res = discardFieldBlock(headerBlock);
    if (res.action == ProcessResult::Action::Continue) {
      res = streamError(streamId, err, ErrorMsg::InvalidStreamStateForHEADERS);
    }
    return res;
  }

  const ErrorCode decodeErr = decodeAndEmitHeaders(streamId, headerBlock, endStream);
  if (decodeErr != ErrorCode::NoError) [[unlikely]] {
    if (decodeErr == ErrorCode::EnhanceYourCalm) {
      return streamError(streamId, decodeErr, ErrorMsg::HeaderListTooLarge);
    }
    if (decodeErr == ErrorCode::ProtocolError) {
      return streamError(streamId, decodeErr, ErrorMsg::MalformedFieldSection);
    }
    return connectionError(decodeErr, ErrorMsg::HPACKDecodingFailed);
  }

  if (endStream) {
    // The sink may have closed streams (its response, for instance), whose pruning can move the map entries.
    it = _streams.find(streamId);
    assert(it != _streams.end());
    if (it->second.isClosed()) {
      closeStream(it);
    }
  }

  return ProcessResult{ProcessResult::Action::Continue};
}

ErrorCode Http2Connection::decodeAndEmitHeaders(uint32_t streamId, std::span<const std::byte> headerBlock,
                                                bool endStream) {
  // The decoder writes the fields in a storage independent of the dynamic table (whose entries may be evicted), that
  // the sink may take over.
  const auto decodeResult = _hpackDecoder.decode(headerBlock);

  if (!decodeResult.isSuccess()) {
    if (decodeResult.isCompressionError()) {
      return ErrorCode::CompressionError;
    }
    // A header list above the local limit (decoded only to keep the dynamic table in sync).
    return decodeResult.error == HpackDecoder::DecodeResult::Error::HeaderListTooLarge ? ErrorCode::EnhanceYourCalm
                                                                                       : ErrorCode::ProtocolError;
  }

  recordHpack(false, headerBlock.size(), decodeResult.headerListSize);

  if (_pSink != nullptr) {
    _pSink->onHeadersDecoded(streamId, decodeResult.decodedHeaders, endStream);
  }

  return ErrorCode::NoError;
}

// ============================
// Output helpers
// ============================

void Http2Connection::sendSettings() {
  const SettingsEntry entries[]{
      SettingsEntry{SettingsParameter::HeaderTableSize, _localSettings.headerTableSize},
      // Always 0: a server MUST NOT send 1 (RFC 9113 §6.5.2), and aeronet clients do not accept pushes.
      SettingsEntry{SettingsParameter::EnablePush, 0},
      SettingsEntry{SettingsParameter::MaxConcurrentStreams, _localSettings.maxConcurrentStreams},
      SettingsEntry{SettingsParameter::InitialWindowSize, _localSettings.initialWindowSize},
      SettingsEntry{SettingsParameter::MaxFrameSize, _localSettings.maxFrameSize},
      SettingsEntry{SettingsParameter::MaxHeaderListSize, _localSettings.maxHeaderListSize},
  };

  WriteSettingsFrame(_outputBuffer, entries);
  recordFrame(true, FrameType::Settings, std::size(entries) * 6U);
  _settingsSent = true;
  if (_localSettings.settingsTimeout.count() > 0) {
    _settingsAckDeadline = std::chrono::steady_clock::now() + _localSettings.settingsTimeout;
  }

  // Also send connection-level WINDOW_UPDATE if needed
  if (_localSettings.connectionWindowSize > kDefaultInitialWindowSize) {
    uint32_t increment = _localSettings.connectionWindowSize - kDefaultInitialWindowSize;
    WriteWindowUpdateFrame(_outputBuffer, 0, increment);
    recordFrame(true, FrameType::WindowUpdate, sizeof(increment));
  }
}

void Http2Connection::sendSettingsAck() {
  WriteSettingsAckFrame(_outputBuffer);
  recordFrame(true, FrameType::Settings, 0);
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
