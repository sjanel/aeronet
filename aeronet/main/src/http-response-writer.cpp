#include "aeronet/http-response-writer.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/file.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/http-request-dispatch.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/log.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/platform.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/timedef.hpp"

namespace aeronet {

HttpResponseWriter::HttpResponseWriter(SingleHttpServer& srv, NativeHandle fd, const HttpRequest& request,
                                       bool requestConnClose, Encoding compressionFormat, const CorsPolicy* pCorsPolicy,
                                       std::span<const ResponseMiddleware> routeResponseMiddleware)
    : _server(&srv),
      _request(&request),
      _fd(fd),
      _head(request.method() == http::Method::HEAD),
      _compressionFormat(compressionFormat),
      // 64UL for Transfer-Encoding: chunked, Content-Length and other headers
      _fixedResponse(64UL, http::StatusCodeOK, srv.config().globalHeaders.fullStringWithLastSep()),
      _pCorsPolicy(pCorsPolicy),
      _routeResponseMiddleware(routeResponseMiddleware) {
  HttpResponse::Options opts;
  opts.close(requestConnClose);
  opts.addTrailerHeader(srv.config().addTrailerHeader);
  opts.headMethod(_head);
  opts.setPrepared();
  _fixedResponse._opts = opts;
  if (requestConnClose) {
    _fixedResponse.headerAddLineUnchecked(http::Connection, http::close);
    _fixedResponse._opts.close(false);
  }
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
  if (!http::IsValidHeaderName(name)) [[unlikely]] {
    throw std::invalid_argument("Invalid HTTP header name");
  }
  if (!http::IsValidHeaderValue(value)) [[unlikely]] {
    throw std::invalid_argument("HTTP header value is invalid");
  }
  if (_state != State::Opened) {
    log::warn("Streaming: cannot add header after headers sent");
    return;
  }
  _fixedResponse.headerAddLine(name, value);
}

void HttpResponseWriter::header(std::string_view name, std::string_view value) {
  if (!http::IsValidHeaderName(name)) [[unlikely]] {
    throw std::invalid_argument("Invalid HTTP header name");
  }
  if (!http::IsValidHeaderValue(value)) [[unlikely]] {
    throw std::invalid_argument("HTTP header value is invalid");
  }
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
    log::warn("Streaming: contentLength ignored fd # {} requestedLen={} reason={}", _fd, len, reason);
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
  const bool addVary = _compressionActivated && _server->_config.compression.addVaryAcceptEncodingHeader;

  if (addContentEncoding) {
    neededSize += HttpResponse::HeaderSize(http::ContentEncoding.size(), GetEncodingStr(_compressionFormat).size());
  }
  if (addVary) {
    neededSize += HttpResponse::HeaderSize(http::Vary.size(), http::AcceptEncoding.size());
  }
  if (chunked()) {
    neededSize += HttpResponse::HeaderSize(http::TransferEncoding.size(), http::chunked.size());
  } else if (!_fixedResponse.hasBodyFile()) {
    neededSize += HttpResponse::HeaderSize(http::ContentLength.size(), nchars(_declaredLength));
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

  // Transfer-Encoding or Content-Length header depending on the case
  if (chunked()) {
    _fixedResponse.headerAddLineUnchecked(http::TransferEncoding, http::chunked);
  } else if (!_fixedResponse.hasBodyFile()) {
    _fixedResponse.headerAddLineUnchecked(http::ContentLength, std::string_view(IntegralToCharVector(_declaredLength)));
  }

  ApplyResponseMiddleware(*_request, _fixedResponse, _routeResponseMiddleware,
                          _server->_router.globalResponseMiddleware(), _server->_telemetry, true,
                          _server->_callbacks.middlewareMetrics);

  if (_pCorsPolicy != nullptr) {
    (void)_pCorsPolicy->applyToResponse(*_request, _fixedResponse);
    _pCorsPolicy = nullptr;
  }

  auto cnxIt = _server->_connections.active.find(_fd);
  _server->queueData(cnxIt, _fixedResponse.finalizeForHttp1(SysClock::now(), http::HTTP_1_1, _fixedResponse._opts,
                                                            nullptr, _server->config().minCapturedBodySize));
  if (cnxIt->second->isAnyCloseRequested()) {
    _state = HttpResponseWriter::State::Failed;
    log::error("Streaming: failed to enqueue headers fd # {} err={} msg={}", _fd, LastSystemError(),
               SystemErrorMessage(LastSystemError()));
    return;
  }
  _state = HttpResponseWriter::State::HeadersSent;
}

void HttpResponseWriter::emitLastChunk() {
  if (!chunked() || _head || _state == State::Failed) {
    return;
  }

  // Emit final chunk with optional trailers (RFC 7230 §4.1.2):
  //   0\r\n
  //   [trailer-name: value\r\n]*
  //   \r\n
  if (_trailers.empty()) {
    _trailers.ensureAvailableCapacity(http::EndChunk.size());
    _trailers.unchecked_append(http::EndChunk);
  } else {
    _trailers.unchecked_append(http::CRLF);  // Final blank line (memory already reserved)
  }

  if (!enqueue(HttpResponseData(std::move(_trailers)))) [[unlikely]] {
    _state = HttpResponseWriter::State::Failed;
    log::error("Streaming: failed enqueuing last chunk fd # {} err={} msg={}", _fd, LastSystemError(),
               SystemErrorMessage(LastSystemError()));
  }
}

bool HttpResponseWriter::writeBody(std::string_view data) {
  if (data.empty()) {
    return true;
  }
  if (_state == State::Ended || _state == State::Failed) {
    log::warn("Streaming: write ignored fd # {} size={} reason={}", _fd, data.size(),
              _state == State::Failed ? "writer-failed" : "already-ended");
    return false;
  }
  if (_fixedResponse.hasBodyFile()) {
    log::warn("Streaming: write ignored fd # {} size={} reason=sendfile-active", _fd, data.size());
    return false;
  }

  // Threshold-based lazy activation using generic Encoder abstraction.
  // We purposefully delay header emission until we either (a) activate compression and have compressed bytes
  // to send or (b) decide to emit identity data (on end()). This allows us to include the Content-Encoding header
  // reliably when compression triggers mid-stream.
  const auto& compressionConfig = _server->_config.compression;

  if (_compressionFormat != Encoding::none && !_compressionActivated &&
      _preCompressBuffer.size() < compressionConfig.minBytes && !_fixedResponse._opts.hasContentEncoding()) {
    return accumulateInPreCompressBuffer(data);
  }

  ensureHeadersSent();

  if (_activeEncoderCtx != nullptr && _compressionFormat != Encoding::none) {
    // CRLF size will be needed for chunked framing.
    const auto additionalCapacity = chunked() ? http::CRLF.size() : 0UL;
    RawChars compressedBuffer(_activeEncoderCtx->minEncodeChunkCapacity(data.size()) + additionalCapacity);
    const auto result =
        _activeEncoderCtx->encodeChunk(data, compressedBuffer.capacity() - additionalCapacity, compressedBuffer.data());
    if (result.hasError()) [[unlikely]] {
      _state = HttpResponseWriter::State::Failed;
      return false;
    }
    const auto written = result.written();
    if (written > 0) {
      compressedBuffer.setSize(written);
      return tryPush(std::move(compressedBuffer));
    }
    return true;
  }
  if (chunked()) {
    // chunked without compression - the size is known so we can write the chunk prefix before the actual body.
    const auto nbDigitsHex = hex_digits(data.size());
    const auto totalSize = nbDigitsHex + http::CRLF.size() + data.size() + http::CRLF.size();

    RawChars chunkBuffer(totalSize);

    char* insertPtr = to_lower_hex(data.size(), chunkBuffer.data());
    insertPtr = Append(http::CRLF, insertPtr);
    insertPtr = Append(data, insertPtr);
    insertPtr = Append(http::CRLF, insertPtr);

    chunkBuffer.setSize(totalSize);

    return tryPush(std::move(chunkBuffer), true);
  }
  return tryPush(RawChars(data));
}

void HttpResponseWriter::trailerAddLine(std::string_view name, std::string_view value) {
  if (!http::IsValidHeaderName(name)) [[unlikely]] {
    throw std::invalid_argument("Invalid HTTP header name");
  }
  if (!http::IsValidHeaderValue(value)) [[unlikely]] {
    throw std::invalid_argument("HTTP header value is invalid");
  }
  if (_state == State::Ended || _state == State::Failed) {
    log::warn("Streaming: trailerAddLine ignored fd # {} name={} reason={}", _fd, name,
              _state == State::Failed ? "writer-failed" : "already-ended");
    return;
  }
  if (_fixedResponse.hasBodyFile()) {
    log::warn("Streaming: trailerAddLine ignored fd # {} name={} reason=sendfile-active", _fd, name);
    return;
  }
  if (!chunked()) {
    log::warn("Streaming: trailerAddLine ignored fd # {} name={} reason=fixed-length-response (contentLength was set)",
              _fd, name);
    return;
  }

  const std::size_t lineSize = HttpResponse::HeaderSize(name.size(), value.size());

  if (_trailers.empty()) {
    _trailers.ensureAvailableCapacityExponential(lineSize + 1UL + http::DoubleCRLF.size());
    _trailers.unchecked_push_back('0');
    _trailers.unchecked_append(http::CRLF);
  } else {
    _trailers.ensureAvailableCapacityExponential(lineSize + http::CRLF.size());
  }

  WriteHeaderCRLF(name, value, _trailers.data() + _trailers.size());

  _trailers.addSize(lineSize);
}

void HttpResponseWriter::end() {
  if (_state == State::Ended || _state == State::Failed) {
    log::debug("Streaming: end ignored fd # {} reason={}", _fd,
               _state == State::Failed ? "writer-failed" : "already-ended");
    return;
  }
  if (_fixedResponse.hasBodyFile()) {
    ensureHeadersSent();
    if (_state != State::Failed) {
      _state = State::Ended;
    }
    return;
  }
  // If compression was delayed and threshold reached earlier, write() already emitted headers and compressed data.
  // Otherwise we may still have buffered identity bytes (below threshold case) — emit headers now then flush.
  ensureHeadersSent();

  if (_compressionActivated) {
    const std::size_t endChunkSize = _activeEncoderCtx->endChunkSize();
    const std::size_t additionalCapacity = chunked() ? http::CRLF.size() : 0UL;
    // encoders may need several calls to end() to flush all remaining data. We loop until they indicate completion.
    RawChars last;
    while (true) {
      last.ensureAvailableCapacityExponential(endChunkSize + additionalCapacity);
      const auto result =
          _activeEncoderCtx->end(last.availableCapacity() - additionalCapacity, last.data() + last.size());
      if (result.hasError()) [[unlikely]] {
        _state = HttpResponseWriter::State::Failed;
        return;
      }
      const auto written = result.written();
      if (written == 0) {
        break;
      }
      last.addSize(written);
    }
    if (!tryPush(std::move(last))) {
      return;
    }
  } else {
    // Identity path; emit headers now (they may not have been sent yet due to delayed strategy) then flush buffered.
    if (!_preCompressBuffer.empty() && !tryPush(std::move(_preCompressBuffer))) {
      return;
    }
  }

  emitLastChunk();
  // If a failure was already recorded, keep it; otherwise mark ended.
  if (_state != State::Failed) {
    _state = HttpResponseWriter::State::Ended;
  }
#ifndef NDEBUG
  // Debug-only protocol correctness check: if a fixed Content-Length was declared, assert body byte count match.
  if (!chunked() && !_head) {
    // _declaredLength may be zero either because user explicitly set it or because we synthesized 0 for HEAD;
    // for HEAD we suppress body so skip. For identity path we track bytesWritten; for compression path we cannot
    // validate because encoder output size may differ from raw input; in that case we only asserted the user should
    // not declare a length unless they know the final encoded size. We still assert if compression was never
    // activated (identity or user-supplied encoding) that counts match.
    if (!_compressionActivated || _compressionFormat == Encoding::none) {
      assert(_bytesWritten == _declaredLength && "Declared Content-Length does not match bytes written");
    }
  }
#endif
}

bool HttpResponseWriter::enqueue(HttpResponseData httpResponseData) {
  // Access the connection state to determine backpressure / closure.
  const auto cnxIt = _server->_connections.active.find(_fd);
  if (cnxIt == _server->_connections.active.end()) {
    return false;
  }
  _server->queueData(cnxIt, std::move(httpResponseData));
  return !cnxIt->second->isAnyCloseRequested();
}

bool HttpResponseWriter::file(File fileObj, std::uint64_t offset, std::uint64_t length, std::string_view contentType) {
  if (_state != State::Opened) {
    log::warn("Streaming: file ignored fd # {} reason=writer-not-open", _fd);
    return false;
  }
  if (_bytesWritten > 0) {
    log::warn("Streaming: file ignored fd # {} reason=body-bytes-already-written", _fd);
    return false;
  }
  if (_declaredLength != 0) {
    log::warn("Streaming: file overriding previously declared Content-Length fd # {}", _fd);
    _declaredLength = 0;
  }
  _compressionFormat = Encoding::none;
  _compressionActivated = false;
  _preCompressBuffer.clear();

  _fixedResponse.file(std::move(fileObj), offset, length, contentType);
  _declaredLength = _fixedResponse.bodyLength();
  return true;
}

bool HttpResponseWriter::accumulateInPreCompressBuffer(std::string_view data) {
  const auto& compressionConfig = _server->_config.compression;
  // Accumulate data into the pre-compression buffer up to minBytes. Always buffer the entire incoming data until
  // we cross the threshold (or end() is called).
  const auto additionalChunkedCapacity = (chunked() ? http::CRLF.size() : 0UL);
  _preCompressBuffer.ensureAvailableCapacityExponential(data.size() + additionalChunkedCapacity);
  _preCompressBuffer.unchecked_append(data);
  if (_preCompressBuffer.size() < compressionConfig.minBytes) {
    // Still below threshold; do not emit headers/body yet.
    return true;
  }
  // Threshold reached exactly or exceeded: activate encoder.
  _activeEncoderCtx = _server->_compressionState.makeContext(_compressionFormat);

  RawChars compressedBuffer(_activeEncoderCtx->minEncodeChunkCapacity(_preCompressBuffer.size()) +
                            additionalChunkedCapacity);
  const auto result = _activeEncoderCtx->encodeChunk(
      _preCompressBuffer, compressedBuffer.capacity() - additionalChunkedCapacity, compressedBuffer.data());
  if (result.hasError()) [[unlikely]] {
    _state = HttpResponseWriter::State::Failed;
    return false;
  }

  const auto written = result.written();

  compressedBuffer.setSize(written);

  _compressionActivated = true;

  ensureHeadersSent();

  _preCompressBuffer.clear();
  if (written > 0) {
    return tryPush(std::move(compressedBuffer));
  }
  return true;
}

bool HttpResponseWriter::tryPush(RawChars data, bool doNotWriteHexPrefix) {
  assert(_state != State::Failed);
  assert(_state != State::Ended);
  if (_head) {
    return true;
  }
  const auto dataSize = data.size();

  HttpResponseData responseData;

  if (doNotWriteHexPrefix || !chunked()) {
    responseData = HttpResponseData(std::move(data));
  } else {
    const auto nbDigitsHex = hex_digits(dataSize);

    RawChars prefix(nbDigitsHex + http::CRLF.size());

    to_lower_hex(dataSize, prefix.data());
    Copy(http::CRLF, prefix.data() + nbDigitsHex);
    prefix.setSize(nbDigitsHex + http::CRLF.size());

    // do not use append here to avoid exponential growth of the buffer
    // capacity of the additional CRLF is already reserved
    assert(data.availableCapacity() >= http::CRLF.size());
    data.unchecked_append(http::CRLF);

    // use the dual buffers with writev to avoid a memmove (a small allocation + writev with dual buffers is probably
    // cheaper than a big memmove)
    responseData = HttpResponseData(std::move(prefix), HttpPayload(std::move(data)));
  }

  if (!enqueue(std::move(responseData))) [[unlikely]] {
    _state = HttpResponseWriter::State::Failed;
    log::error("Streaming: failed enqueuing coalesced chunk fd # {} err={} msg={}", _fd, LastSystemError(),
               SystemErrorMessage(LastSystemError()));
    return false;
  }
  _bytesWritten += dataSize;
  return true;
}

}  // namespace aeronet
