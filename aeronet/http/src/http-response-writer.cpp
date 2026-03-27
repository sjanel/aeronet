#include "aeronet/http-response-writer.hpp"

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
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/writer-transport.hpp"

namespace aeronet {

HttpResponseWriter::HttpResponseWriter(internal::IWriterTransport& transport, const HttpRequest& request,
                                       Encoding compressionFormat, const CompressionConfig& compressionConfig,
                                       internal::ResponseCompressionState& compressionState,
                                       std::string_view globalHeadersStr, bool addTrailerHeader)
    : _transport(&transport),
      _request(&request),
      _head(request.method() == http::Method::HEAD),
      _compressionFormat(compressionFormat),
      // 64UL for Transfer-Encoding: chunked, Content-Length and other headers
      _fixedResponse(64UL, http::StatusCodeOK, globalHeadersStr),
      _pCompressionConfig(&compressionConfig),
      _pCompressionState(&compressionState) {
  HttpResponse::Options opts;
  opts.addTrailerHeader(addTrailerHeader);
  opts.headMethod(_head);
  opts.setPrepared();
  _fixedResponse._opts = opts;
  _fixedResponse.headerAddLineUnchecked(http::ContentType, http::ContentTypeApplicationOctetStream);
}

void HttpResponseWriter::status(http::StatusCode code) {
  if (_state != State::Opened) {
    log::warn("Streaming: cannot set status after headers sent");
    return;
  }
  _fixedResponse.status(code);
}

void HttpResponseWriter::reason(std::string_view reason) {
  if (_state != State::Opened) {
    log::warn("Streaming: cannot set reason after headers sent");
    return;
  }
  _fixedResponse.reason(reason);
}

void HttpResponseWriter::headerAddLine(std::string_view name, std::string_view value) {
  if (_state != State::Opened) {
    log::warn("Streaming: cannot add header after headers sent");
    return;
  }
  _fixedResponse.headerAddLine(name, value);
}

void HttpResponseWriter::header(std::string_view name, std::string_view value) {
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

  std::string_view oldCT = _fixedResponse.headerValueOrEmpty(http::ContentType);
  assert(!oldCT.empty());
  _fixedResponse.overrideHeaderUnchecked(oldCT.data(), oldCT.data() + oldCT.size(), ct);
}

void HttpResponseWriter::contentLength(std::size_t len) {
  if (_state != State::Opened) {
    std::string_view reason;
    if (_state == State::Failed) {
      reason = "writer-failed";
    } else if (_state == State::HeadersSent) {
      reason = "headers-already-sent";
    } else {
      reason = "unknown";
    }
    log::warn("Streaming: contentLength ignored {} requestedLen={} reason={}", _transport->logId(), len, reason);
    return;
  }
  _declaredLength = len;
}

void HttpResponseWriter::ensureHeadersSent() {
  if (_state != State::Opened) {
    return;
  }

  // Compute needed header size and reserve capacity in the fixed response buffer to have at most 1 allocation.
  std::size_t neededSize = 0UL;

  const bool addContentEncoding = _compressionActivated && !_fixedResponse._opts.hasContentEncoding();
  const bool addVary = _compressionActivated && _pCompressionConfig->addVaryAcceptEncodingHeader;

  if (addContentEncoding) {
    neededSize += HttpResponse::HeaderSize(http::ContentEncoding.size(), GetEncodingStr(_compressionFormat).size());
  }
  if (addVary) {
    neededSize += HttpResponse::HeaderSize(http::Vary.size(), http::AcceptEncoding.size());
  }

  _fixedResponse._data.ensureAvailableCapacity(neededSize);

  // If compression already activated (delayed strategy) but header not sent yet, add Content-Encoding now.
  if (addContentEncoding) {
    _fixedResponse.headerAddLineUnchecked(http::ContentEncoding, GetEncodingStr(_compressionFormat));
    _fixedResponse._opts.setHasContentEncoding(true);
  }
  if (addVary) {
    _fixedResponse.headerAppendValue(http::Vary, http::AcceptEncoding);
  }

  if (!_transport->emitHeaders(_fixedResponse, *_request, _compressionActivated, _compressionFormat, _declaredLength,
                               _head)) {
    _state = State::Failed;
    log::error("Streaming: failed to emit headers {}", _transport->logId());
  } else {
    _state = State::HeadersSent;
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
  if (_fixedResponse.hasBodyFile()) {
    log::warn("Streaming: write ignored {} size={} reason=sendfile-active", _transport->logId(), data.size());
    return false;
  }

  // Threshold-based lazy activation using generic Encoder abstraction.
  // We purposefully delay header emission until we either (a) activate compression and have compressed bytes
  // to send or (b) decide to emit identity data (on end()). This allows us to include the Content-Encoding header
  // reliably when compression triggers mid-stream.
  if (_compressionFormat != Encoding::none && !_compressionActivated &&
      _preCompressBuffer.size() < _pCompressionConfig->minBytes && !_fixedResponse._opts.hasContentEncoding()) {
    return accumulateInPreCompressBuffer(data);
  }

  ensureHeadersSent();
  if (_state == State::Failed) {
    return false;
  }

  if (_activeEncoderCtx != nullptr && _compressionFormat != Encoding::none) {
    RawChars compressedBuffer(_activeEncoderCtx->minEncodeChunkCapacity(data.size()));
    const auto result = _activeEncoderCtx->encodeChunk(data, compressedBuffer.capacity(), compressedBuffer.data());
    if (result.hasError()) [[unlikely]] {
      _state = State::Failed;
      return false;
    }
    const auto written = result.written();
    if (written > 0) {
      compressedBuffer.setSize(written);
      if (!_head && !_transport->emitData(compressedBuffer)) {
        _state = State::Failed;
        return false;
      }
#ifndef NDEBUG
      _bytesWritten += written;
#endif
    }
    return true;
  }

  // Uncompressed path
  if (!_head && !_transport->emitData(data)) {
    _state = State::Failed;
    log::error("Streaming: failed emitting data {} size={}", _transport->logId(), data.size());
    return false;
  }
#ifndef NDEBUG
  _bytesWritten += data.size();
#endif
  return true;
}

void HttpResponseWriter::trailerAddLine(std::string_view name, std::string_view value) {
  if (!http::IsValidHeaderName(name)) [[unlikely]] {
    throw std::invalid_argument("Invalid HTTP header name");
  }
  if (!http::IsValidHeaderValue(value)) [[unlikely]] {
    throw std::invalid_argument("HTTP header value is invalid");
  }
  if (_state == State::Ended || _state == State::Failed) {
    log::warn("Streaming: trailerAddLine ignored {} name={} reason={}", _transport->logId(), name,
              _state == State::Failed ? "writer-failed" : "already-ended");
    return;
  }
  if (_fixedResponse.hasBodyFile()) {
    log::warn("Streaming: trailerAddLine ignored {} name={} reason=sendfile-active", _transport->logId(), name);
    return;
  }

  const std::size_t lineSize = HttpResponse::HeaderSize(name.size(), value.size());

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
  // Otherwise we may still have buffered identity bytes (below threshold case) — emit headers now then flush.
  ensureHeadersSent();
  if (_state == State::Failed) {
    return;
  }

  if (_fixedResponse.hasBodyFile()) {
    _state = State::Ended;
    return;
  }

  if (_compressionActivated) {
    const std::size_t endChunkSize = _activeEncoderCtx->endChunkSize();
    // encoders may need several calls to end() to flush all remaining data. We loop until they indicate completion.
    RawChars last;
    while (true) {
      last.ensureAvailableCapacityExponential(endChunkSize);
      const auto result = _activeEncoderCtx->end(last.availableCapacity(), last.data() + last.size());
      if (result.hasError()) [[unlikely]] {
        _state = State::Failed;
        return;
      }
      const auto written = result.written();
      if (written == 0) {
        break;
      }
      last.addSize(written);
    }
    if (!_head && !last.empty() && !_transport->emitData(last)) {
      _state = State::Failed;
      return;
    }
  } else {
    // Identity path; emit headers now (they may not have been sent yet due to delayed strategy) then flush buffered.
    if (!_head && !_preCompressBuffer.empty()) {
      if (!_transport->emitData(_preCompressBuffer)) {
        _state = State::Failed;
        return;
      }
      _preCompressBuffer.clear();
    }
  }

  if (!_transport->emitEnd(std::move(_trailers))) {
    _state = State::Failed;
    log::error("Streaming: failed emitting end {}", _transport->logId());
    return;
  }

  _state = State::Ended;
#ifndef NDEBUG
  // Debug-only protocol correctness check: if a fixed Content-Length was declared, assert body byte count match.
  if (!_head && _declaredLength != 0 && (!_compressionActivated || _compressionFormat == Encoding::none)) {
    assert(_bytesWritten == _declaredLength && "Declared Content-Length does not match bytes written");
  }
#endif
}

bool HttpResponseWriter::file(File file, std::uint64_t offset, std::uint64_t length, std::string_view contentType) {
  if (_state != State::Opened) {
    log::warn("Streaming: file ignored {} reason=writer-not-open", _transport->logId());
    return false;
  }
  assert(_bytesWritten == 0);
  if (_declaredLength != 0) {
    log::warn("Streaming: file overriding previously declared Content-Length {}", _transport->logId());
    _declaredLength = 0;
  }
  _compressionFormat = Encoding::none;
  _compressionActivated = false;
  _preCompressBuffer.clear();

  _fixedResponse.file(std::move(file), offset, length, contentType);
  _declaredLength = _fixedResponse.bodyLength();
  return true;
}

bool HttpResponseWriter::accumulateInPreCompressBuffer(std::string_view data) {
  // Accumulate data into the pre-compression buffer up to minBytes. Always buffer the entire incoming data until
  // we cross the threshold (or end() is called).
  _preCompressBuffer.ensureAvailableCapacityExponential(data.size());
  _preCompressBuffer.unchecked_append(data);
  if (_preCompressBuffer.size() < _pCompressionConfig->minBytes) {
    // Still below threshold; do not emit headers/body yet.
    return true;
  }
  // Threshold reached exactly or exceeded: activate encoder.
  _activeEncoderCtx = _pCompressionState->makeContext(_compressionFormat);

  RawChars compressedBuffer(_activeEncoderCtx->minEncodeChunkCapacity(_preCompressBuffer.size()));
  const auto result =
      _activeEncoderCtx->encodeChunk(_preCompressBuffer, compressedBuffer.capacity(), compressedBuffer.data());
  if (result.hasError()) [[unlikely]] {
    _state = State::Failed;
    return false;
  }

  const auto written = result.written();

  compressedBuffer.setSize(written);

  _compressionActivated = true;

  ensureHeadersSent();
  if (_state == State::Failed) {
    return false;
  }

  _preCompressBuffer.clear();
  if (!_head && written > 0 && !_transport->emitData(compressedBuffer)) {
    _state = State::Failed;
    return false;
  }
  return true;
}

}  // namespace aeronet
