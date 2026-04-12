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
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-stream.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/timedef.hpp"
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
  Http2WriterTransport(Http2Connection& connection, uint32_t streamId, const ConcatenatedHeaders* pGlobalHeaders)
      : _pConnection(&connection), _pGlobalHeaders(pGlobalHeaders), _streamId(streamId) {}

  bool emitHeaders(HttpResponse& response, const HttpRequest& /*request*/, bool /*compressionActivated*/,
                   Encoding /*compressionFormat*/, std::size_t /*declaredLength*/, bool isHead) override {
    _isHead = isHead;

    // HTTP/2 requires lowercase header names
    response.finalizeForHttp2();

    // Finalize Date header (same as sendResponse path)
    WriteCRLFDateHeader(SysClock::now(), response._data.data() + response.headersStartPos());

    // Determine END_STREAM: headers-only response if HEAD request, or no body expected and no trailers.
    // For streaming, we generally do NOT set END_STREAM on HEADERS because body follows.
    // However, if isHead is true, end() will be called but no body data is sent.
    // We delay END_STREAM to emitEnd() since the writer always calls end().
    static constexpr bool kEndStream = false;

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
      _pendingBuffer.ensureAvailableCapacityExponential(data.size());
      _pendingBuffer.unchecked_append(data);
      return true;
    }

    // Try sending directly.
    const auto bytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(data.data()), data.size());
    const ErrorCode err = _pConnection->sendData(_streamId, bytes, /*endStream=*/false);

    if (err == ErrorCode::NoError) {
      return true;
    }

    if (err == ErrorCode::FlowControlError) {
      // Flow control window exhausted - buffer data for later flushing.
      _pendingBuffer.ensureAvailableCapacityExponential(data.size());
      _pendingBuffer.unchecked_append(data);
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
      _pendingTrailers = std::move(trailers);
      _pendingEnd = true;
      return true;
    }

    // All data was sent inline - emit the stream end now.
    if (_isHead) {
      // HEAD: send empty DATA frame with END_STREAM
      const ErrorCode err = _pConnection->sendData(_streamId, {}, /*endStream=*/true);
      if (err != ErrorCode::NoError) {
        log::error("HTTP/2 streaming: failed to send END_STREAM on stream {}: {}", _streamId, ErrorCodeName(err));
        return false;
      }
      return true;
    }

    if (!trailers.empty()) {
      // Emit trailers as a HEADERS frame with END_STREAM
      const ErrorCode err =
          _pConnection->sendHeaders(_streamId, http::StatusCode{}, HeadersView(trailers), /*endStream=*/true);
      if (err != ErrorCode::NoError) {
        log::error("HTTP/2 streaming: failed to send trailers on stream {}: {}", _streamId, ErrorCodeName(err));
        return false;
      }
      return true;
    }

    // No trailers - send empty DATA frame with END_STREAM
    const ErrorCode err = _pConnection->sendData(_streamId, {}, /*endStream=*/true);
    if (err != ErrorCode::NoError) {
      log::error("HTTP/2 streaming: failed to send END_STREAM on stream {}: {}", _streamId, ErrorCodeName(err));
      return false;
    }
    return true;
  }

  [[nodiscard]] bool isAlive() const override {
    const Http2Stream* pStream = _pConnection->getStream(_streamId);
    return pStream != nullptr && pStream->canSend();
  }

  [[nodiscard]] uint32_t logId() const override { return _streamId; }

  // ============================
  // Post-handler state extraction
  // ============================

  /// Whether any data/file/end is pending (needs deferred flushing by the protocol handler).
  [[nodiscard]] bool hasPendingData() const noexcept { return !_pendingBuffer.empty() || _pendingEnd; }

  /// Whether a file payload was extracted from the response (needs PendingFileSend handling).
  [[nodiscard]] bool hasPendingFile() const noexcept { return _pendingFile; }

  FilePayload extractPendingFile() noexcept {
    _pendingFile = false;
    return {std::move(_filePayload.file), _filePayload.offset, _filePayload.length};
  }

  RawChars extractPendingBuffer() noexcept { return std::move(_pendingBuffer); }
  RawChars extractPendingTrailers() noexcept { return std::move(_pendingTrailers); }

 private:
  Http2Connection* _pConnection;
  const ConcatenatedHeaders* _pGlobalHeaders;
  uint32_t _streamId;
  bool _isHead{false};

  // Flow-control buffering
  bool _pendingFile{false};
  bool _pendingEnd{false};
  RawChars _pendingBuffer;
  RawChars _pendingTrailers;

  // File payload extracted from response
  FilePayload _filePayload;
};

}  // namespace aeronet::http2
