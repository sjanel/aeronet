#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/file-payload.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-error-code-name.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/writer-transport.hpp"

namespace aeronet::http2 {

/// HTTP/2 transport backend for HttpResponseWriter.
/// Emits HEADERS and DATA frames on an HTTP/2 stream, buffering data when
/// flow-control windows are exhausted.
///
/// After the streaming handler returns, the caller must check hasPendingData()
/// and transfer any remaining buffer/trailers/file into the protocol handler's
/// pending-send maps for deferred flushing.
class Http2WriterTransport final : public internal::IWriterTransport {
 public:
  Http2WriterTransport(Http2Connection& connection, uint32_t streamId, const ConcatenatedHeaders* pGlobalHeaders,
                       const char* cachedDateHeader, std::size_t existingDeferredBytes,
                       uint32_t maxConnectionPendingBytes, uint32_t maxStreamPendingBytes)
      : _pConnection(&connection),
        _pGlobalHeaders(pGlobalHeaders),
        _pCachedDateHeader(cachedDateHeader),
        _existingDeferredBytes(existingDeferredBytes),
        _maxConnectionPendingBytes(maxConnectionPendingBytes),
        _maxStreamPendingBytes(maxStreamPendingBytes),
        _streamId(streamId) {}

  bool emitHeaders(HttpResponse& response, const HttpRequestView& /*request*/, bool /*compressionActivated*/,
                   Encoding /*compressionFormat*/, std::size_t /*declaredLength*/, bool isHead) override {
    _isHead = isHead;

    response.finalizeHeadersAndBody();

    // Finalize Date header (same as sendResponse path).
    CopyCRLFDateHeader(_pCachedDateHeader, response._data.data() + response.dateHeaderStartPos());

    // Determine END_STREAM: headers-only response if HEAD request, or no body expected and no trailers.
    // For streaming, we generally do NOT set END_STREAM on HEADERS because body follows.
    // However, if isHead is true, end() will be called but no body data is sent.
    // We delay END_STREAM to emitEnd() since the writer always calls end().
    static constexpr bool kEndStream = false;

    std::size_t headerBytes = FrameHeader::kSize;
    const auto addHeaderBytes = [this, &headerBytes](std::size_t size) {
      if (headerBytes > _maxConnectionPendingBytes || size > _maxConnectionPendingBytes - headerBytes) {
        return false;
      }
      headerBytes += size;
      return true;
    };
    const std::size_t globalHeadersSize =
        _pGlobalHeaders == nullptr ? 0 : _pGlobalHeaders->fullStringWithLastSep().size();
    if (!addHeaderBytes(response.headersFlatViewWithDate().size()) || !addHeaderBytes(globalHeadersSize) ||
        !hasConnectionCapacity(headerBytes)) {
      markOverflow();
      return false;
    }

    const ErrorCode err = _pConnection->sendHeaders(
        _streamId, response.status(), HeadersView(response.headersFlatViewWithDate()), kEndStream, _pGlobalHeaders);
    if (err != ErrorCode::NoError) {
      log::error("HTTP/2 streaming: failed to send headers on stream {}: {}", _streamId, ErrorCodeName(err));
      return false;
    }

    // If the response carries a file payload, extract it for deferred sending.
    if (auto* fp = response.filePayloadPtr(); fp != nullptr && !isHead) {
      _pendingFile = true;
      _filePayload = std::move(*fp);
    }

    return true;
  }

  bool emitData(std::string_view data) override {
    assert(!data.empty() && !_isHead);

    // If we already have buffered data from a previous flow-control stall, just append.
    if (!_pendingBuffer.empty()) {
      if (!canBuffer(data.size())) {
        markOverflow();
        return false;
      }
      _pendingBuffer.append(data);
      return true;
    }

    if (!hasConnectionCapacity(framedDataSize(data.size()))) {
      markOverflow();
      return false;
    }

    // Try sending directly.
    const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size());
    const ErrorCode err = _pConnection->sendData(_streamId, bytes, /*endStream=*/false);

    if (err == ErrorCode::NoError) {
      return true;
    }

    if (err == ErrorCode::FlowControlError) {
      // Flow control window exhausted - buffer data for later flushing.
      if (data.size() > _maxStreamPendingBytes) {
        markOverflow();
        return false;
      }
      _pendingBuffer.append(data);
      return true;
    }

    // Fatal stream error
    log::error("HTTP/2 streaming: failed to send data on stream {}: {}", _streamId, ErrorCodeName(err));
    return false;
  }

  bool emitEnd(RawChars trailers) override {
    // File payloads are handled by HttpResponseWriter::end() which early-returns before calling emitEnd().
    // The protocol handler reads pending file state directly from extractPendingFile().
    assert(!_pendingFile && "emitEnd should not be called when a file payload is pending");

    if (!_pendingBuffer.empty()) {
      // We have buffered data that couldn't be sent due to flow control.
      // Store trailers for the protocol handler to send later.
      if (!canBuffer(trailers.size())) {
        markOverflow();
        return false;
      }
      _pendingTrailers = std::move(trailers);
      _pendingEnd = true;
      return true;
    }

    // All data was sent inline - emit the stream end now.
    if (_isHead) {
      // HEAD: send empty DATA frame with END_STREAM
      if (!hasConnectionCapacity(FrameHeader::kSize)) {
        markOverflow();
        return false;
      }
      const ErrorCode err = _pConnection->sendData(_streamId, std::span<const std::byte>{}, /*endStream=*/true);
      if (err != ErrorCode::NoError) {
        log::error("HTTP/2 streaming: failed to send END_STREAM on stream {}: {}", _streamId, ErrorCodeName(err));
        return false;
      }
      return true;
    }

    if (!trailers.empty()) {
      // Emit trailers as a HEADERS frame with END_STREAM
      if (!hasConnectionCapacity(trailers.size() + FrameHeader::kSize)) {
        markOverflow();
        return false;
      }
      const ErrorCode err =
          _pConnection->sendHeaders(_streamId, http::StatusCode{}, HeadersView(trailers), /*endStream=*/true);
      if (err != ErrorCode::NoError) {
        log::error("HTTP/2 streaming: failed to send trailers on stream {}: {}", _streamId, ErrorCodeName(err));
        return false;
      }
      return true;
    }

    // No trailers - send empty DATA frame with END_STREAM
    if (!hasConnectionCapacity(FrameHeader::kSize)) {
      markOverflow();
      return false;
    }
    const ErrorCode err = _pConnection->sendData(_streamId, std::span<const std::byte>{}, /*endStream=*/true);
    if (err != ErrorCode::NoError) {
      log::error("HTTP/2 streaming: failed to send END_STREAM on stream {}: {}", _streamId, ErrorCodeName(err));
      return false;
    }
    return true;
  }

  [[nodiscard]] uint32_t logId() const override { return _streamId; }

  // ============================
  // Post-handler state extraction
  // ============================

  /// Whether any data/file/end is pending (needs deferred flushing by the protocol handler).
  [[nodiscard]] bool hasPendingData() const noexcept { return !_pendingBuffer.empty() || _pendingEnd; }

  /// Whether a file payload was extracted from the response (needs PendingFileSend handling).
  [[nodiscard]] bool hasPendingFile() const noexcept { return _pendingFile; }

  [[nodiscard]] bool overflowed() const noexcept { return _overflowed; }

  FilePayload extractPendingFile() noexcept {
    _pendingFile = false;
    return {std::move(_filePayload.file), _filePayload.offset, _filePayload.length};
  }

  RawChars extractPendingBuffer() noexcept { return std::move(_pendingBuffer); }
  RawChars extractPendingTrailers() noexcept { return std::move(_pendingTrailers); }

 private:
  [[nodiscard]] std::size_t framedDataSize(std::size_t payloadSize) const noexcept {
    const std::size_t maxFrameSize = _pConnection->peerSettings().maxFrameSize;
    const std::size_t frameCount = (payloadSize + maxFrameSize - 1U) / maxFrameSize;
    return payloadSize + (frameCount * FrameHeader::kSize);
  }

  [[nodiscard]] bool hasConnectionCapacity(std::size_t additionalBytes) const noexcept {
    const std::size_t retained =
        _pConnection->pendingOutputSize() + _existingDeferredBytes + _pendingBuffer.size() + _pendingTrailers.size();
    return additionalBytes <= _maxConnectionPendingBytes && retained <= _maxConnectionPendingBytes - additionalBytes;
  }

  [[nodiscard]] bool canBuffer(std::size_t additionalBytes) const noexcept {
    const std::size_t streamPending = _pendingBuffer.size() + _pendingTrailers.size();
    return additionalBytes <= _maxStreamPendingBytes && streamPending <= _maxStreamPendingBytes - additionalBytes &&
           hasConnectionCapacity(additionalBytes);
  }

  void markOverflow() noexcept {
    _overflowed = true;
    _pendingBuffer.clear();
    _pendingTrailers.clear();
    _pendingEnd = false;
  }

  Http2Connection* _pConnection;
  const ConcatenatedHeaders* _pGlobalHeaders;
  const char* _pCachedDateHeader;
  std::size_t _existingDeferredBytes;
  uint32_t _maxConnectionPendingBytes;
  uint32_t _maxStreamPendingBytes;
  uint32_t _streamId;
  bool _isHead{false};

  // Flow-control buffering
  bool _pendingFile{false};
  bool _pendingEnd{false};
  bool _overflowed{false};
  RawChars _pendingBuffer;
  RawChars _pendingTrailers;

  // File payload extracted from response
  FilePayload _filePayload;
};

}  // namespace aeronet::http2
