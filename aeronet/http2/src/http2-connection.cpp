#include "aeronet/http2-connection.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-headers-view.hpp"
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
#include "aeronet/metric-label.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/simple-charconv.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/tolower-str.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/vector.hpp"
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

// Copying a tiny body beside its frame header lets one transport write cover a batch of
// responses. Larger bodies keep their owned allocation and gather-write without a copy.
constexpr std::size_t kMaxInlineDataFrameCopySize = 256;

constexpr uint64_t HpackHeaderFieldSize(std::string_view name, std::string_view value) noexcept {
  constexpr uint64_t kEntryOverhead = 32;
  return name.size() + value.size() + kEntryOverhead;
}

constexpr std::string_view FrameTypeName(FrameType type) noexcept {
  switch (type) {
    case FrameType::Data:
      return "data";
    case FrameType::Headers:
      return "headers";
    case FrameType::Priority:
      return "priority";
    case FrameType::RstStream:
      return "rst_stream";
    case FrameType::Settings:
      return "settings";
    case FrameType::PushPromise:
      return "push_promise";
    case FrameType::Ping:
      return "ping";
    case FrameType::GoAway:
      return "goaway";
    case FrameType::WindowUpdate:
      return "window_update";
    case FrameType::Continuation:
      return "continuation";
    default:
      return "unknown";
  }
}

}  // namespace

// ============================
// Queued output
// ============================

Http2Connection::OutputBlock::OutputBlock(RawBytes&& data, std::size_t offset) noexcept
    : _payload(std::move(data)), _payloadOffset(offset) {
  assert(offset <= _payload.size());
  _payloadSize = _payload.size() - offset;
  _remainingSize = _payloadSize;
}

Http2Connection::OutputBlock::OutputBlock(RawBytes&& owner, std::size_t dataOffset, std::size_t dataSize,
                                          uint32_t maxFrameSize, uint32_t streamId, bool endStream, bool headers)
    : _payload(std::move(owner)),
      _payloadOffset(dataOffset),
      _payloadSize(dataSize),
      _maxFrameSize(maxFrameSize),
      _framed(true) {
  assert(dataOffset <= _payload.size());
  assert(dataSize <= _payload.size() - dataOffset);
  assert(maxFrameSize != 0);

  const auto frameCount = static_cast<uint32_t>(dataSize == 0 ? 1 : ((dataSize + maxFrameSize - 1U) / maxFrameSize));
  _remainingSize = dataSize + (static_cast<std::size_t>(frameCount) * FrameHeader::kSize);
  if (frameCount > 1U) {
    const std::size_t continuationHeadersSize = (static_cast<std::size_t>(frameCount) - 1U) * FrameHeader::kSize;
    _continuationHeaders = RawBytes(continuationHeadersSize);
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

    std::byte* pHeaderData =
        first ? _firstHeader.data() : _continuationHeaders.data() + ((frameIndex - 1U) * FrameHeader::kSize);
    WriteFrameHeader(pHeaderData, {static_cast<uint32_t>(framePayloadSize(frameIndex)), frameType, flags, streamId});
  }
}

std::size_t Http2Connection::OutputBlock::framePayloadSize(uint32_t frameIndex) const noexcept {
  const std::size_t frameOffset = static_cast<std::size_t>(frameIndex) * _maxFrameSize;
  return std::min(_payloadSize - frameOffset, static_cast<std::size_t>(_maxFrameSize));
}

const std::byte* Http2Connection::OutputBlock::frameHeader(uint32_t frameIndex) const noexcept {
  return frameIndex == 0 ? _firstHeader.data() : _continuationHeaders.data() + ((frameIndex - 1U) * FrameHeader::kSize);
}

std::string_view Http2Connection::OutputBlock::firstFragment() const noexcept {
  assert(!empty());
  if (!_framed) {
    return {reinterpret_cast<const char*>(_payload.data() + _payloadOffset + _framePayloadOffset),
            _payloadSize - _framePayloadOffset};
  }
  if (_headerOffset < FrameHeader::kSize) {
    return {reinterpret_cast<const char*>(frameHeader(_frameIndex) + _headerOffset),
            FrameHeader::kSize - _headerOffset};
  }

  const std::size_t payloadSize = framePayloadSize(_frameIndex);
  assert(_framePayloadOffset < payloadSize);
  const std::size_t frameOffset = static_cast<std::size_t>(_frameIndex) * _maxFrameSize;
  return {reinterpret_cast<const char*>(_payload.data() + _payloadOffset + frameOffset + _framePayloadOffset),
          payloadSize - _framePayloadOffset};
}

vector<std::string_view>::size_type Http2Connection::OutputBlock::pendingFragmentCount() const noexcept {
  assert(!empty());
  if (!_framed) {
    return 1;
  }

  const uint32_t frameCount =
      static_cast<uint32_t>(_payloadSize == 0 ? 1 : ((_payloadSize + _maxFrameSize - 1U) / _maxFrameSize));
  auto fragmentCount = static_cast<vector<std::string_view>::size_type>((frameCount - _frameIndex) * 2U);
  fragmentCount -= static_cast<vector<std::string_view>::size_type>(_headerOffset == FrameHeader::kSize);
  fragmentCount -= static_cast<vector<std::string_view>::size_type>(_payloadSize == 0);
  return fragmentCount;
}

void Http2Connection::OutputBlock::appendFragments(vector<std::string_view>& fragments) const {
  assert(!empty());
  if (!_framed) {
    fragments.emplace_back(reinterpret_cast<const char*>(_payload.data() + _payloadOffset + _framePayloadOffset),
                           _payloadSize - _framePayloadOffset);
    return;
  }

  const uint32_t frameCount =
      static_cast<uint32_t>(_payloadSize == 0 ? 1 : ((_payloadSize + _maxFrameSize - 1U) / _maxFrameSize));
  uint32_t frameIndex = _frameIndex;
  uint32_t framePayloadOffset = _framePayloadOffset;
  uint8_t headerOffset = _headerOffset;

  while (frameIndex < frameCount) {
    if (headerOffset < FrameHeader::kSize) {
      fragments.emplace_back(reinterpret_cast<const char*>(frameHeader(frameIndex) + headerOffset),
                             FrameHeader::kSize - headerOffset);
      headerOffset = FrameHeader::kSize;
    }

    const std::size_t payloadSize = framePayloadSize(frameIndex);
    if (framePayloadOffset < payloadSize) {
      const std::size_t frameOffset = static_cast<std::size_t>(frameIndex) * _maxFrameSize;
      fragments.emplace_back(
          reinterpret_cast<const char*>(_payload.data() + _payloadOffset + frameOffset + framePayloadOffset),
          payloadSize - framePayloadOffset);
    }
    ++frameIndex;
    framePayloadOffset = 0;
    headerOffset = 0;
  }
}

void Http2Connection::OutputBlock::consume(std::size_t bytesWritten) noexcept {
  assert(bytesWritten <= _remainingSize);
  _remainingSize -= bytesWritten;

  if (!_framed) {
    _framePayloadOffset += static_cast<uint32_t>(bytesWritten);
    return;
  }

  const uint32_t frameCount =
      static_cast<uint32_t>(_payloadSize == 0 ? 1 : ((_payloadSize + _maxFrameSize - 1U) / _maxFrameSize));
  while (bytesWritten != 0 && _frameIndex < frameCount) {
    if (_headerOffset < FrameHeader::kSize) {
      const std::size_t consumed = std::min(bytesWritten, FrameHeader::kSize - _headerOffset);
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
  _payload = {};
  _continuationHeaders = {};
}

// ============================
// Constructor / Destructor
// ============================

Http2Connection::Http2Connection(const Http2Config& config, bool isServer, tracing::TelemetryContext* telemetryContext)
    : _localSettings(config),
      _connectionSendWindow(static_cast<int32_t>(kDefaultInitialWindowSize)),
      _connectionRecvWindow(static_cast<int32_t>(config.connectionWindowSize)),
      _hpackEncoder(config.headerTableSize),
      _hpackDecoder(config.headerTableSize, config.mergeUnknownRequestHeaders),
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

Http2Connection::ProcessResult Http2Connection::processInput(std::span<const std::byte> data) {
  return processInput(data, std::numeric_limits<std::size_t>::max());
}

Http2Connection::ProcessResult Http2Connection::processInput(std::span<const std::byte> data,
                                                             std::size_t outputHighWaterMark) {
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
  if (_outputBlockReadPos < _outputBlocks.size()) {
    const std::string_view fragment = _outputBlocks[_outputBlockReadPos].firstFragment();
    return {reinterpret_cast<const std::byte*>(fragment.data()), fragment.size()};
  }
  if (_outputWritePos < _outputBuffer.size()) {
    return {_outputBuffer.data() + _outputWritePos, _outputBuffer.size() - _outputWritePos};
  }
  return {};
}

void Http2Connection::getPendingOutputFragments(vector<std::string_view>& fragments) const {
  fragments.clear();
  auto fragmentCount = static_cast<vector<std::string_view>::size_type>(_outputWritePos < _outputBuffer.size());
  for (auto blockIndex = _outputBlockReadPos; blockIndex < _outputBlocks.size(); ++blockIndex) {
    fragmentCount += _outputBlocks[blockIndex].pendingFragmentCount();
  }
  fragments.reserve(fragmentCount);

  for (auto blockIndex = _outputBlockReadPos; blockIndex < _outputBlocks.size(); ++blockIndex) {
    _outputBlocks[blockIndex].appendFragments(fragments);
  }
  if (_outputWritePos < _outputBuffer.size()) {
    fragments.emplace_back(reinterpret_cast<const char*>(_outputBuffer.data() + _outputWritePos),
                           _outputBuffer.size() - _outputWritePos);
  }
  assert(fragments.size() == fragmentCount);
}

std::size_t Http2Connection::pendingOutputSize() const noexcept {
  return _outputBlocksSize + _outputBuffer.size() - _outputWritePos;
}

void Http2Connection::onOutputWritten(std::size_t bytesWritten) {
  assert(bytesWritten <= pendingOutputSize());

  while (bytesWritten != 0 && _outputBlockReadPos < _outputBlocks.size()) {
    OutputBlock& block = _outputBlocks[_outputBlockReadPos];
    const std::size_t consumed = std::min(bytesWritten, block.remainingSize());
    block.consume(consumed);
    _outputBlocksSize -= consumed;
    bytesWritten -= consumed;
    if (block.empty()) {
      block.release();
      ++_outputBlockReadPos;
    }
  }
  if (_outputBlockReadPos == _outputBlocks.size()) {
    _outputBlocks.clear();
    _outputBlockReadPos = 0;
  }

  if (bytesWritten != 0) {
    _outputWritePos += bytesWritten;
    assert(_outputWritePos <= _outputBuffer.size());
    if (_outputWritePos == _outputBuffer.size()) {
      _outputBuffer.clear();
      _outputWritePos = 0;
    }
  }
}

void Http2Connection::discardPendingOutput() noexcept {
  _outputBlocks.clear();
  _outputBlocksSize = 0;
  _outputBlockReadPos = 0;
  _outputBuffer.clear();
  _outputWritePos = 0;
}

void Http2Connection::sealOutputBuffer() {
  if (_outputWritePos == _outputBuffer.size()) {
    _outputBuffer.clear();
    _outputWritePos = 0;
    return;
  }
  if (!_outputBuffer.empty()) {
    _outputBlocksSize += _outputBuffer.size() - _outputWritePos;
    _outputBlocks.emplace_back(std::move(_outputBuffer), _outputWritePos);
    _outputBuffer = {};
    _outputWritePos = 0;
  }
}

void Http2Connection::queueOutputBlock(OutputBlock block) {
  sealOutputBuffer();
  _outputBlocksSize += block.remainingSize();
  _outputBlocks.push_back(std::move(block));
}

void Http2Connection::initiateGoAway(ErrorCode errorCode, ErrorMsg msg) {
  if (_state == ConnectionState::Closed || _state == ConnectionState::GoAwaySent) {
    return;
  }

  WriteGoAwayFrame(_outputBuffer, _lastPeerStreamId, errorCode, msg);
  recordFrame(true, FrameType::GoAway, 8U + ConvertProcessResultErrorMsgToSv(msg).size());
  _state = ConnectionState::GoAwaySent;
  _goAwayLastStreamId = _lastPeerStreamId;
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

  if (_onStreamClosed) {
    _onStreamClosed(streamId);
  }

  // Don't remove immediately - keep for a short time for late frames.
  _closedStreamsFifo.push_back(streamId);
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
    headerListSize = HpackHeaderFieldSize(http::PseudoHeaderMethod, methodStr) +
                     HpackHeaderFieldSize(http::PseudoHeaderScheme, scheme) +
                     HpackHeaderFieldSize(http::PseudoHeaderAuthority, authority) +
                     HpackHeaderFieldSize(http::PseudoHeaderPath, target);
    _hpackEncoder.encode(_outputBuffer, http::PseudoHeaderMethod, methodStr);
    _hpackEncoder.encode(_outputBuffer, http::PseudoHeaderScheme, scheme);
    _hpackEncoder.encode(_outputBuffer, http::PseudoHeaderAuthority, authority);
    _hpackEncoder.encode(_outputBuffer, http::PseudoHeaderPath, target);
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
  if (dataSize > static_cast<std::size_t>(std::numeric_limits<uint32_t>::max())) {
    return ErrorCode::FlowControlError;
  }

  const auto flowControlledSize = static_cast<uint32_t>(dataSize);
  if (!pStream->consumeSendWindow(flowControlledSize)) {
    return ErrorCode::FlowControlError;
  }
  if (std::cmp_less(_connectionSendWindow, flowControlledSize)) {
    (void)pStream->increaseSendWindow(flowControlledSize);
    return ErrorCode::FlowControlError;
  }
  _connectionSendWindow -= static_cast<int32_t>(flowControlledSize);

  // canSend() above ensures the stream is Open or HalfClosedRemote, which are exactly
  // the states onSendData() handles.
  [[maybe_unused]] const ErrorCode err = pStream->onSendData(endStream);
  assert(err == ErrorCode::NoError);
  return err;
}

ErrorCode Http2Connection::sendData(uint32_t streamId, std::span<const std::byte> data, bool endStream) {
  const ErrorCode err = prepareSendData(streamId, data.size(), endStream);
  if (err != ErrorCode::NoError) {
    return err;
  }

  if (data.size() <= kMaxInlineDataFrameCopySize) {
    WriteDataFrame(_outputBuffer, streamId, data, endStream);
  } else {
    queueDataBlock(RawBytes(data), 0, data.size(), streamId, endStream);
  }
  recordFrame(true, FrameType::Data, data.size(),
              (data.size() + _peerSettings.maxFrameSize - 1U) / _peerSettings.maxFrameSize);
  return ErrorCode::NoError;
}

ErrorCode Http2Connection::sendData(uint32_t streamId, RawBytes&& owner, std::size_t dataOffset, std::size_t dataSize,
                                    bool endStream) {
  assert(dataOffset <= owner.size());
  assert(dataSize + dataOffset <= owner.size());

  const ErrorCode err = prepareSendData(streamId, dataSize, endStream);
  if (err != ErrorCode::NoError) {
    return err;
  }

  assert(dataSize != 0);
  if (dataSize <= kMaxInlineDataFrameCopySize) {
    WriteDataFrame(_outputBuffer, streamId, std::span<const std::byte>(owner.data() + dataOffset, dataSize), endStream);
  } else {
    queueDataBlock(std::move(owner), dataOffset, dataSize, streamId, endStream);
  }
  recordFrame(true, FrameType::Data, dataSize,
              (dataSize + _peerSettings.maxFrameSize - 1U) / _peerSettings.maxFrameSize);
  return ErrorCode::NoError;
}

void Http2Connection::sendRstStream(uint32_t streamId, ErrorCode errorCode) {
  WriteRstStreamFrame(_outputBuffer, streamId, errorCode);
  recordFrame(true, FrameType::RstStream, sizeof(uint32_t));

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
                                                              std::size_t outputHighWaterMark) {
  std::size_t totalConsumed = 0;

  while (data.size() >= FrameHeader::kSize) {
    if (pendingOutputSize() >= outputHighWaterMark) {
      break;
    }
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

    recordFrame(false, header.type, header.length);

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

  // Replenish a receive window only after half its configured credit has been consumed. Restoring the
  // full credit at once keeps large transfers moving while avoiding two tiny WINDOW_UPDATE frames for
  // every DATA frame. A stream-level update is useless after END_STREAM, but connection-level credit is
  // retained for later streams on the same connection.
  if (_onData && payloadSize > 0) {
    const int32_t initialStreamWindow = static_cast<int32_t>(_localSettings.initialWindowSize);
    if (!frame.endStream && it->second.recvWindow() <= initialStreamWindow / 2) {
      sendWindowUpdate(header.streamId, static_cast<uint32_t>(initialStreamWindow - it->second.recvWindow()));
    }

    const int32_t initialConnectionWindow = static_cast<int32_t>(_localSettings.connectionWindowSize);
    if (_connectionRecvWindow <= initialConnectionWindow / 2) {
      sendWindowUpdate(0, static_cast<uint32_t>(initialConnectionWindow - _connectionRecvWindow));
    }
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

  auto [it, inserted] = _streams.try_emplace(header.streamId, header.streamId, _peerSettings.initialWindowSize,
                                             _localSettings.initialWindowSize);
  if (inserted) {
    // Validate stream ID
    if (_isServer) {
      // Client-initiated streams must be odd and increasing
      if ((header.streamId & 1U) == 0) [[unlikely]] {
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
    recordStreamOpened(false);
    _lastPeerStreamId = header.streamId;
  }
  Http2Stream* pStream = &it->second;

  // Handle priority if present
  if (frame.hasPriority) {
    if (frame.streamDependency == header.streamId) [[unlikely]] {
      return streamError(header.streamId, ErrorCode::ProtocolError, ErrorMsg::StreamDependsOnItself);
    }
    const uint32_t effectiveDependency = priorityDepthExceeded(frame.streamDependency) ? 0U : frame.streamDependency;
    pStream->setPriority(effectiveDependency, frame.weight, frame.exclusive);
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
    if (decodeErr == ErrorCode::EnhanceYourCalm) {
      return streamError(header.streamId, decodeErr, ErrorMsg::HeaderListTooLarge);
    }
    if (decodeErr == ErrorCode::ProtocolError) {
      return streamError(header.streamId, decodeErr, ErrorMsg::MalformedFieldSection);
    }
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
    const uint32_t effectiveDependency = priorityDepthExceeded(frame.streamDependency) ? 0U : frame.streamDependency;
    pStream->setPriority(effectiveDependency, frame.weight, frame.exclusive);
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
    recordFrame(true, FrameType::Ping, sizeof(frame.opaqueData));
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
    if (decodeErr == ErrorCode::EnhanceYourCalm) {
      return streamError(_headerBlockStreamId, decodeErr, ErrorMsg::HeaderListTooLarge);
    }
    if (decodeErr == ErrorCode::ProtocolError) {
      return streamError(_headerBlockStreamId, decodeErr, ErrorMsg::MalformedFieldSection);
    }
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
                                    bool endStream, std::size_t oldSize, const ConcatenatedHeaders* pGlobalHeaders,
                                    uint64_t headerListSize) {
  assert(statusCode == 0 || statusCode == http::MagicForHttpRequest || (statusCode >= 100 && statusCode <= 999));

  // Encode :status pseudo-header first if present
  if (statusCode >= 100) {
    char statusBuf[3];
    writeStatusCode(statusBuf, statusCode);
    const std::string_view statusStr(statusBuf, sizeof(statusBuf));
    headerListSize += HpackHeaderFieldSize(http::PseudoHeaderStatus, statusStr);
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
    const std::string_view trimmedValue = TrimOws(value);
    headerListSize += HpackHeaderFieldSize(name, trimmedValue);
    _hpackEncoder.encode(_outputBuffer, name, trimmedValue);
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

      headerListSize += HpackHeaderFieldSize(headerName, headerValue);
      _hpackEncoder.encode(_outputBuffer, headerName, headerValue);
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
    RawBytes owner(std::move(_outputBuffer));
    _outputBuffer = {};
    _outputWritePos = 0;
    queueOutputBlock(OutputBlock::Headers(std::move(owner), oldSize, headerBlockSize, _peerSettings.maxFrameSize,
                                          streamId, endStream));
  } else {
    // Preserve older batched frames ahead of this oversized header block. Oversized
    // fields are rare, so copying only this block is preferable to sealing every normal
    // response into a separate transport fragment.
    RawBytes owner(headerBlockSize);
    owner.unchecked_append(
        std::span<const std::byte>(_outputBuffer.data() + oldSize, static_cast<std::size_t>(headerBlockSize)));
    _outputBuffer.setSize(outputSizeBeforeHeaders);
    queueOutputBlock(
        OutputBlock::Headers(std::move(owner), 0, headerBlockSize, _peerSettings.maxFrameSize, streamId, endStream));
  }
  recordFrame(true, FrameType::Headers, _peerSettings.maxFrameSize);
  recordFrame(true, FrameType::Continuation, continuationBytes,
              (continuationBytes + _peerSettings.maxFrameSize - 1U) / _peerSettings.maxFrameSize);
}

ErrorCode Http2Connection::decodeAndEmitHeaders(uint32_t streamId, std::span<const std::byte> headerBlock,
                                                bool endStream) {
  // Collect decoded headers into an intermediate storage. We always build decodedHeaders
  // because we will invoke the new decoded-headers callback if set. Note: We must copy
  // strings here because the HPACK dynamic table may evict entries during decode,
  // invalidating string_views that point to evicted entries.

  const auto decodeResult = _hpackDecoder.decode(headerBlock);

  if (!decodeResult.isSuccess()) {
    return decodeResult.isCompressionError() ? ErrorCode::CompressionError : ErrorCode::ProtocolError;
  }

  recordHpack(false, headerBlock.size(), decodeResult.headerListSize);

  if (decodeResult.headerListSize > _localSettings.maxHeaderListSize) {
    return ErrorCode::EnhanceYourCalm;
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
  recordFrame(true, FrameType::Settings, std::size(entries) * 6U);
  _settingsSent = true;

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
