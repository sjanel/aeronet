#include "aeronet/http-response-writer.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/file.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/http-message-common.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/reserved-headers.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/writer-transport.hpp"

namespace aeronet {

HttpResponseWriter::HttpResponseWriter(IWriterTransport& transport, const HttpRequestView& request, Encoding encoding,
                                       const CompressionConfig& compressionConfig, CompressionState& compressionState,
                                       std::string_view globalHeadersStr, HttpMessage::Options opts)
    : _transport(&transport),
      _request(&request),
      _noContent(request.method() == http::Method::HEAD),
      _encoding(encoding),
      // 64UL for Transfer-Encoding: chunked, Content-Length and other headers
      _fixedResponse(64UL, http::StatusCodeOK, globalHeadersStr, std::string_view{}, std::string_view{}, opts),
      _declaredLength(IWriterTransport::kNoDeclaredLength),
      _pCompressionConfig(&compressionConfig),
      _pCompressionState(&compressionState) {
  _fixedResponse.headerAddLineUnchecked(http::ContentType, http::ContentTypeApplicationOctetStream);
}

void HttpResponseWriter::status(http::StatusCode code) {
  if (_state != State::Opened) {
    log::warn("Streaming: cannot set status after headers sent");
    return;
  }
  _statusCode = code;
  _fixedResponse.status(code);
}

void HttpResponseWriter::reason(std::string_view reason) {
  if (_state != State::Opened) {
    log::warn("Streaming: cannot set reason after headers sent");
    return;
  }
  _fixedResponse.reason(reason);
}

void HttpResponseWriter::headerAddLine(LowerAsciiKey name, std::string_view value) {
  if (_state != State::Opened) {
    log::warn("Streaming: cannot add header after headers sent");
    return;
  }
  _fixedResponse.headerAddLine(name, value);
}

void HttpResponseWriter::header(LowerAsciiKey name, std::string_view value) {
  if (_state != State::Opened) {
    log::warn("Streaming: cannot add header after headers sent");
    return;
  }
  _fixedResponse.header(name, value);
}

void HttpResponseWriter::contentType(std::string_view ct) {
  if (_state != State::Opened) {
    log::warn("Streaming: cannot set content-type after headers sent");
    return;
  }
  ct = CheckContentType(ct);

  // Not using headerImpl / other header public methods to avoid std::invalid_argument on ContentType header set without
  // the body.
  std::string_view oldCT = _fixedResponse.headerValueOrEmpty(http::ContentType);
  assert(!oldCT.empty());
  _fixedResponse.overrideHeaderUnchecked(oldCT.data(), oldCT.data() + oldCT.size(), ct);
}

void HttpResponseWriter::contentLength(std::size_t len) {
  // Body bytes buffered below the compression threshold count as written: the call comes too late, as once they are
  // sent.
  if (_state != State::Opened || !_preCompressBuffer.empty()) {
    std::string_view reason;
    if (_state == State::Failed) {
      reason = "writer-failed";
    } else if (_state == State::HeadersSent) {
      reason = "headers-already-sent";
    } else if (_state == State::Opened) {
      reason = "body-already-written";
    } else {
      reason = "already-ended";
    }
    log::warn("Streaming: contentLength ignored {} requestedLen={} reason={}", _transport->logId(), len, reason);
    return;
  }
  _declaredLength = len;
}

void HttpResponseWriter::ensureHeadersSent(bool bodyWritten) {
  if (_state != State::Opened) {
    return;
  }

  if (!http::StatusCodeAllowsBody(_statusCode)) {
    dropContent();
  } else if (_declaredLength != IWriterTransport::kNoDeclaredLength && _encoding != Encoding::none &&
             !_fixedResponse._opts.hasContentEncoding()) {
    // The declared length is the one of the body written by the handler: the threshold is checked against it, without
    // buffering, and the same decision is taken for a HEAD request, whatever the handler writes for it. An empty body
    // (0) is never compressed, minBytes being at least 1.
    assert(_activeEncoderCtx == nullptr && _preCompressBuffer.empty());
    if (_declaredLength >= _pCompressionConfig->minBytes) {
      startCompressionIfAllowed();
    } else {
      _encoding = Encoding::none;
    }
  }

  // Compute needed header size and reserve capacity in the fixed response buffer to have at most 1 allocation.
  std::size_t neededSize = 0UL;

  const bool isCompressionActive = _activeEncoderCtx != nullptr;
  const bool addContentEncoding = isCompressionActive && !_fixedResponse._opts.hasContentEncoding();
  const bool addVary =
      _pCompressionConfig->addVaryAcceptEncodingHeader && (isCompressionActive || identityVariantVaries(bodyWritten));

  if (addContentEncoding) {
    // The prefix of a weakened ETag included, without searching for it.
    neededSize +=
        http::HeaderSize(http::ContentEncoding.size(), GetEncodingStr(_encoding).size()) + http::WeakETagPrefix.size();
  }
  if (addVary) {
    neededSize += http::HeaderSize(http::Vary.size(), http::AcceptEncoding.size());
  }

  _fixedResponse.reserve(_fixedResponse.sizeInlined() + neededSize);

  // If compression already activated (delayed strategy) but header not sent yet, add Content-Encoding now.
  if (addContentEncoding) {
    _fixedResponse.headerAddLineUnchecked(http::ContentEncoding, GetEncodingStr(_encoding));
    _fixedResponse._opts.setHasContentEncoding();
    _fixedResponse.weakenStrongETag();
  }
  if (addVary) {
    _fixedResponse.headerAppendValueIfAbsent(http::Vary, http::AcceptEncoding);
  }

  if (!_transport->emitHeaders(_fixedResponse, *_request, _declaredLength, _noContent)) {
    _state = State::Failed;
    log::error("Streaming: failed to emit headers {}", _transport->logId());
  } else {
    _state = State::HeadersSent;
    // From now on, _declaredLength counts the body bytes still due (a file is sent by the transport).
    _fixedLength = _declaredLength != IWriterTransport::kNoDeclaredLength && !_noContent && !_isFile;
  }
}

bool HttpResponseWriter::identityVariantVaries([[maybe_unused]] bool bodyWritten) const {
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
  if (!_pCompressionConfig->addVaryAcceptEncodingToUncompressed ||
      _request->responsePossibleEncoding() != Encoding::none || _isFile || _fixedResponse._opts.hasContentEncoding() ||
      _statusCode == http::StatusCodePartialContent || !http::StatusCodeAllowsBody(_statusCode)) {
    return false;
  }
  const bool largeEnough = _declaredLength != IWriterTransport::kNoDeclaredLength
                               ? _declaredLength >= _pCompressionConfig->minBytes
                               : bodyWritten;
  return largeEnough && HttpCodec::IsContentTypeAllowed(*_pCompressionConfig, _fixedResponse);
#else
  return false;
#endif
}

void HttpResponseWriter::startCompressionIfAllowed() {
  // The Content-Range of a 206 describes the bytes of the content sent (RFC 9110 §14.4): it is never encoded.
  if (_statusCode != http::StatusCodePartialContent &&
      HttpCodec::IsContentTypeAllowed(*_pCompressionConfig, _fixedResponse)) {
    _activeEncoderCtx = _pCompressionState->makeContext(_encoding);
    // The length of the encoded body is not known when the head is sent (RFC 9110 §8.6): a declared length, the one
    // of the body before its encoding, is dropped, and the response is chunked (close-delimited for HTTP/1.0).
    _declaredLength = IWriterTransport::kNoDeclaredLength;
  } else {
    _encoding = Encoding::none;
  }
}

void HttpResponseWriter::dropContent() {
  // 1xx, 204, 205 and 304 responses have no content (RFC 9110 §6.4.1, §15.3.6): whatever the handler writes from now
  // on is dropped, as for a HEAD request. The head carries no Content-Type, Content-Encoding nor Transfer-Encoding, and
  // no trailers. Only the HTTP/1.1 head of a 205 has a Content-Length, of 0 (see the transport).
  _noContent = true;
  _encoding = Encoding::none;
  _activeEncoderCtx = nullptr;
  _preCompressBuffer.clear();
  _trailers.clear();
  _declaredLength = IWriterTransport::kNoDeclaredLength;
  if (_isFile) {
    // The file comes with its body headers.
    _fixedResponse.body(std::string_view{});
    _isFile = false;
  } else {
    // The Content-Type placeholder set at construction.
    const std::string_view placeholder = _fixedResponse.headerValueOrEmpty(http::ContentType);
    _fixedResponse.eraseHeaderLine(http::ContentType, placeholder.data(), placeholder.data() + placeholder.size());
  }
}

bool HttpResponseWriter::writeBody(std::string_view data) {
  if (data.empty()) {
    return true;
  }
  if (_state == State::Ended || _state == State::Failed) {
    log::warn("Streaming: write ignored {} size={} reason={}", _transport->logId(), data.size(),
              _state == State::Failed ? "writer-failed" : "already-ended");
    return false;
  }
  if (_isFile) {
    log::warn("Streaming: write ignored {} size={} reason=sendfile-active", _transport->logId(), data.size());
    return false;
  }

  // The data buffered until the compression threshold, once handed over to the encoder or the transport: it is
  // released when this write returns, and end() has nothing left to emit, whatever the path taken below.
  RawChars bufferedData;

  // Threshold-based lazy activation using generic Encoder abstraction.
  // We purposefully delay header emission until we either (a) activate compression and have compressed bytes
  // to send or (b) decide to emit identity data (on end()). This allows us to include the Content-Encoding header
  // reliably when compression triggers mid-stream.
  // A declared length gives the size of the body at once: the decision is taken when the head is sent, see
  // ensureHeadersSent().
  if (_encoding != Encoding::none && _activeEncoderCtx == nullptr &&
      _declaredLength == IWriterTransport::kNoDeclaredLength &&
      _preCompressBuffer.size() < _pCompressionConfig->minBytes && !_fixedResponse._opts.hasContentEncoding()) {
    // Accumulate data into the pre-compression buffer up to minBytes. Always buffer the entire incoming data until
    // we cross the threshold (or end() is called).

    if (_preCompressBuffer.empty() && data.size() >= _pCompressionConfig->minBytes) {
      // Optim without copy - directly activate the encoder if the incoming data already meets the threshold.
    } else {
      // Buffer the incoming data until we reach the compression threshold.
      _preCompressBuffer.append(data);
      if (_preCompressBuffer.size() < _pCompressionConfig->minBytes) {
        // Still below threshold; do not emit headers/body yet.
        return true;
      }
      bufferedData = std::move(_preCompressBuffer);
      data = bufferedData;
    }

    // Threshold reached exactly or exceeded: activate the encoder, unless the content type is not eligible, the data
    // being then sent as is, from this write on.
    startCompressionIfAllowed();
  }

  ensureHeadersSent(true);
  if (_state == State::Failed) {
    return false;
  }
  if (_noContent) {
    // The decision to compress, made above, gives the headers of the GET response: the body itself is not sent.
    return true;
  }

  if (_activeEncoderCtx != nullptr) {
    assert(_encoding != Encoding::none);
    // The output of one write is emitted at once: when it does not fit (a brotli or zstd burst), the buffer grows, as
    // in end(), and keeps its capacity for the next writes.
    _compressedBuffer.clear();
    do {
      _compressedBuffer.ensureAvailableCapacityExponential(_activeEncoderCtx->minEncodeChunkCapacity(data.size()));
      const auto result =
          _activeEncoderCtx->encodeChunk(data, _compressedBuffer.availableCapacity(), _compressedBuffer.end());
      if (result.hasError()) [[unlikely]] {
        _state = State::Failed;
        log::error("Streaming: compression failed {}", _transport->logId());
        return false;
      }
      _compressedBuffer.addSize(result.writtenIfNoError());
    } while (!data.empty());
    if (_compressedBuffer.empty()) {
      // This happens regularly - the compressors often keeps data in their own internal buffers before any flush
      // occurs.
      return true;
    }
    data = _compressedBuffer;
  }

  if (_fixedLength) {
    // Identity body (a compressed one has no declared length).
    if (data.size() > _declaredLength) [[unlikely]] {
      return writeBeyondDeclaredLength(data);
    }
    _declaredLength -= data.size();
  }
  if (!_transport->emitData(data)) {
    _state = State::Failed;
    log::error("Streaming: failed emitting data {} size={}", _transport->logId(), data.size());
    return false;
  }
  return true;
}

bool HttpResponseWriter::writeBeyondDeclaredLength(std::string_view data) {
  // RFC 9110 §8.6: the bytes beyond the declared Content-Length would be read by the client as the start of the next
  // response of the connection. Only the declared ones are sent, and the writer fails: the connection is closed after
  // them (HTTP/1.1), the stream reset (HTTP/2).
  log::error("Streaming: write of {} bytes exceeds the declared Content-Length by {} {}, the excess is dropped",
             data.size(), data.size() - _declaredLength, _transport->logId());
  const std::string_view due = data.substr(0, _declaredLength);
  _declaredLength = 0;
  _state = State::Failed;
  if (!due.empty() && !_transport->emitData(due)) {
    log::error("Streaming: failed emitting data {} size={}", _transport->logId(), due.size());
  }
  return false;
}

void HttpResponseWriter::trailerAddLine(LowerAsciiKey name, std::string_view value) {
  if (!http::IsValidLowerCaseHeaderName(name)) [[unlikely]] {
    throw std::invalid_argument("Invalid trailer header name (a token, in lower case)");
  }
  // RFC 9110 §6.5.1, as for HttpResponse::trailerAddLine().
  if (http::IsForbiddenTrailerHeader(name)) [[unlikely]] {
    throw std::invalid_argument("This field is not allowed in trailers");
  }
  value = TrimOws(value);
  if (!http::IsValidHeaderValue(value)) [[unlikely]] {
    throw std::invalid_argument("HTTP header value is invalid");
  }
  if (_state == State::Ended || _state == State::Failed) {
    log::warn("Streaming: trailerAddLine ignored {} name={} reason={}", _transport->logId(), name.get(),
              _state == State::Failed ? "writer-failed" : "already-ended");
    return;
  }
  if (_isFile) {
    log::warn("Streaming: trailerAddLine ignored {} name={} reason=sendfile-active", _transport->logId(), name.get());
    return;
  }

  const std::size_t lineSize = http::HeaderSize(name.size(), value.size());

  _trailers.ensureAvailableCapacityExponential(lineSize + http::CRLF.size());

  WriteHeaderCRLF(name, value, _trailers.data() + _trailers.size());

  _trailers.addSize(lineSize);
}

void HttpResponseWriter::end() {
  if (_state == State::Ended || _state == State::Failed) {
    log::debug("Streaming: end ignored {} reason={}", _transport->logId(),
               _state == State::Failed ? "writer-failed" : "already-ended");
    return;
  }
  // If compression was delayed and threshold reached earlier, write() already emitted headers and compressed data.
  // Otherwise we may still have buffered identity bytes (below threshold case) - emit headers now then flush.
  ensureHeadersSent(false);
  if (_state == State::Failed) {
    return;
  }

  if (_isFile) {
    _state = State::Ended;
    return;
  }

  if (_activeEncoderCtx != nullptr && !_noContent) {
    // encoders may need several calls to end() to flush all remaining data. We loop until they indicate completion.
    // A call after one that filled its room gets as much room as already written (see
    // HttpMessage::finalizeInlineBody()): the end of a brotli stream may be several MiB.
    std::size_t room = _activeEncoderCtx->endChunkSize();
    _compressedBuffer.clear();
    while (true) {
      _compressedBuffer.ensureAvailableCapacityExponential(room);
      const std::size_t givenRoom = _compressedBuffer.availableCapacity();
      const auto result = _activeEncoderCtx->end(givenRoom, _compressedBuffer.end());
      if (result.hasError()) [[unlikely]] {
        _state = State::Failed;
        return;
      }
      const auto written = result.writtenIfNoError();
      if (written == 0) {
        break;
      }
      _compressedBuffer.addSize(written);
      if (written == givenRoom) {
        room = std::max<std::size_t>(room, _compressedBuffer.size());
      }
    }
    if (!_compressedBuffer.empty() && !_transport->emitData(_compressedBuffer)) {
      _state = State::Failed;
      return;
    }
  } else if (!_noContent && !_preCompressBuffer.empty()) {
    // Identity path; emit headers now (they may not have been sent yet due to delayed strategy) then flush buffered.
    if (!_transport->emitData(_preCompressBuffer)) {
      _state = State::Failed;
      return;
    }
    _preCompressBuffer.clear();
  }

  if (_fixedLength && _declaredLength != 0) [[unlikely]] {
    // A body shorter than its declared Content-Length: the client would wait for the rest, or read the next response of
    // the connection as such. The writer fails, so that the connection is closed after what was sent (HTTP/1.1), the
    // stream reset (HTTP/2): the client knows that the response is incomplete.
    log::error("Streaming: body ended {} bytes short of the declared Content-Length {}", _declaredLength,
               _transport->logId());
    _state = State::Failed;
    return;
  }

  if (!_transport->emitEnd(std::move(_trailers))) {
    _state = State::Failed;
    log::error("Streaming: failed emitting end {}", _transport->logId());
    return;
  }

  _state = State::Ended;
}

bool HttpResponseWriter::file(File file, std::uint64_t offset, std::uint64_t length, std::string_view contentType) {
  if (_state != State::Opened) {
    log::warn("Streaming: file ignored {} reason=writer-not-open", _transport->logId());
    return false;
  }
  if (_declaredLength != IWriterTransport::kNoDeclaredLength) {
    log::warn("Streaming: file overriding previously declared Content-Length {}", _transport->logId());
  }
  _encoding = Encoding::none;
  _activeEncoderCtx = nullptr;
  _preCompressBuffer.clear();

  // The file body comes with its own body headers: the Content-Type placeholder set at construction (or by
  // contentType()) must not stay as a second Content-Type. Erased after the call, which may throw.
  const bool hasContentTypePlaceholder = !_fixedResponse.hasBodyHeaders();
  _fixedResponse.file(std::move(file), offset, length, contentType);
  if (hasContentTypePlaceholder) {
    // The placeholder line is before the body headers, so it is the first Content-Type found.
    const std::string_view placeholder = _fixedResponse.headerValueOrEmpty(http::ContentType);
    _fixedResponse.eraseHeaderLine(http::ContentType, placeholder.data(), placeholder.data() + placeholder.size());
  }
  _declaredLength = _fixedResponse.bodyLength();
  _isFile = true;
  return true;
}

}  // namespace aeronet
