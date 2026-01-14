#include "aeronet/http-response-writer.hpp"

#include <cassert>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/compression-config.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/encoder.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/file.hpp"
#include "aeronet/header-write.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/log.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/timedef.hpp"

#ifndef NDEBUG
#include <system_error>
#endif

namespace aeronet {

HttpResponseWriter::HttpResponseWriter(SingleHttpServer& srv, int fd, const HttpRequest& request, bool headRequest,
                                       bool requestConnClose, Encoding compressionFormat, const CorsPolicy* pCorsPolicy,
                                       std::span<const ResponseMiddleware> routeResponseMiddleware)
    : _server(&srv),
      _request(&request),
      _fd(fd),
      _head(headRequest),
      _requestConnClose(requestConnClose),
      _compressionFormat(compressionFormat),
      _pCorsPolicy(pCorsPolicy),
      _routeResponseMiddleware(routeResponseMiddleware) {}

void HttpResponseWriter::status(http::StatusCode code) {
  if (_state != State::Opened) {
    log::warn("Streaming: cannot set status after headers sent");
    return;
  }
  _fixedResponse.status(code);
}

void HttpResponseWriter::status(http::StatusCode code, std::string_view reason) {
  if (_state != State::Opened) {
    log::warn("Streaming: cannot set status after headers sent");
    return;
  }
  _fixedResponse.status(code, reason);
}

void HttpResponseWriter::headerAddLine(std::string_view name, std::string_view value) {
  if (_state != State::Opened) {
    log::warn("Streaming: cannot add header after headers sent");
    return;
  }
  if (CaseInsensitiveEqual(http::ContentEncoding, name)) {
    _contentEncodingHeaderPresent = true;
  }
  _fixedResponse.headerAddLine(name, value);
}

void HttpResponseWriter::header(std::string_view name, std::string_view value) {
  if (_state != State::Opened) {
    log::warn("Streaming: cannot add header after headers sent");
    return;
  }
  if (CaseInsensitiveEqual(http::ContentEncoding, name)) {
    _contentEncodingHeaderPresent = true;
  }
  _fixedResponse.header(name, value);
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
  _fixedResponse.setHeader(http::ContentLength, std::string_view(IntegralToCharVector(len)));
}

void HttpResponseWriter::ensureHeadersSent() {
  if (_state != State::Opened) {
    return;
  }
  // For HEAD requests never emit chunked framing; force zero Content-Length if not provided.
  if (chunked()) {
    _fixedResponse.headerAddLine(http::TransferEncoding, "chunked");
  } else if (!_fixedResponse.hasBodyFile() && _declaredLength == 0) {
    _fixedResponse.headerAddLine(http::ContentLength, "0");
  }

  // If Content-Type has not been set, set to 'application/octet-stream' by default.
  _fixedResponse.setHeader(http::ContentType, http::ContentTypeApplicationOctetStream, HttpResponse::OnlyIfNew::Yes);
  // If compression already activated (delayed strategy) but header not sent yet, add Content-Encoding now.
  if (_compressionActivated) {
    _fixedResponse.setHeader(http::ContentEncoding, GetEncodingStr(_compressionFormat));
    if (_server->_config.compression.addVaryAcceptEncodingHeader) {
      _fixedResponse.headerAppendValue(http::Vary, http::AcceptEncoding);
    }
  }
  _server->applyResponseMiddleware(*_request, _fixedResponse, _routeResponseMiddleware, true);

  if (_pCorsPolicy != nullptr) {
    (void)_pCorsPolicy->applyToResponse(*_request, _fixedResponse);
    _pCorsPolicy = nullptr;
  }

  auto cnxIt = _server->_connections.active.find(_fd);
  if (cnxIt == _server->_connections.active.end() ||
      !_server->queueFormattedHttp1Response(
          cnxIt, _fixedResponse.finalizeForHttp1(SysClock::now(), http::HTTP_1_1, _requestConnClose,
                                                 _server->config().globalHeaders, _head,
                                                 _server->config().minCapturedBodySize))) {
    _state = HttpResponseWriter::State::Failed;
    log::error("Streaming: failed to enqueue headers fd # {} errno={} msg={}", _fd, errno, std::strerror(errno));
    return;
  }
  _state = HttpResponseWriter::State::HeadersSent;
}

void HttpResponseWriter::emitChunk(std::string_view data) {
  if (_head || _state == State::Failed) {
    return;
  }

  // enough for 64-bit length in hex + CRLF
  static constexpr std::size_t kMaxHexLen = 2UL * sizeof(uint64_t);

  RawChars chunkBuf(kMaxHexLen + http::CRLF.size() + data.size() + http::CRLF.size());

  auto res = std::to_chars(chunkBuf.data(), chunkBuf.data() + kMaxHexLen, static_cast<uint64_t>(data.size()), 16);
  assert(res.ec == std::errc());
  std::memcpy(res.ptr, http::CRLF.data(), http::CRLF.size());
  chunkBuf.setSize(static_cast<std::size_t>(res.ptr + http::CRLF.size() - chunkBuf.data()));

  chunkBuf.unchecked_append(data);
  chunkBuf.unchecked_append(http::CRLF);
  if (!enqueue(HttpResponseData(std::move(chunkBuf)))) [[unlikely]] {
    _state = HttpResponseWriter::State::Failed;
    log::error("Streaming: failed enqueuing coalesced chunk fd # {} errno={} msg={}", _fd, errno, std::strerror(errno));
    return;
  }
  _bytesWritten += data.size();
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
    _trailers.ensureAvailableCapacity(1UL + http::DoubleCRLF.size());
    _trailers.unchecked_push_back('0');
    _trailers.unchecked_append(http::DoubleCRLF);
  } else {
    _trailers.unchecked_append(http::CRLF);  // Final blank line (memory already reserved)
  }

  if (!enqueue(HttpResponseData(std::move(_trailers)))) [[unlikely]] {
    _state = HttpResponseWriter::State::Failed;
    log::error("Streaming: failed enqueuing last chunk fd # {} errno={} msg={}", _fd, errno, std::strerror(errno));
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
      _preCompressBuffer.size() < compressionConfig.minBytes && !_contentEncodingHeaderPresent) {
    return accumulateInPreCompressBuffer(data);
  }

  ensureHeadersSent();

  if (_activeEncoderCtx) {
    data = _activeEncoderCtx->encodeChunk(data);
    if (data.empty()) {
      return true;
    }
  }

  if (chunked()) {
    emitChunk(data);
  } else if (!_head) {
    if (!enqueue(HttpResponseData(data))) [[unlikely]] {
      _state = HttpResponseWriter::State::Failed;
      log::error("Streaming: failed enqueuing fixed body fd # {} errno={} msg={}", _fd, errno, std::strerror(errno));
      return false;
    }
    _bytesWritten += data.size();
  }
  return _state != State::Failed;  // backpressure signaled via connection close flag, failure sets failed state
}

void HttpResponseWriter::trailerAddLine(std::string_view name, std::string_view value) {
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

  WriteHeaderCRLF(_trailers.data() + _trailers.size(), name, value);

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
    auto last = _activeEncoderCtx->encodeChunk(std::string_view{});
    if (!last.empty()) {
      if (chunked()) {
        emitChunk(last);
      } else if (!_head) {
        if (!enqueue(HttpResponseData(last))) {
          _state = HttpResponseWriter::State::Failed;
          log::error("Streaming: failed enqueuing final encoder output fd # {} errno={} msg={}", _fd, errno,
                     std::strerror(errno));
          return;
        }
      }
    }
  } else {
    // Identity path; emit headers now (they may not have been sent yet due to delayed strategy) then flush buffered.
    if (!_preCompressBuffer.empty()) {
      if (chunked()) {
        emitChunk(_preCompressBuffer);
      } else if (!_head) {
        if (!enqueue(HttpResponseData(std::move(_preCompressBuffer)))) {
          _state = HttpResponseWriter::State::Failed;
          log::error("Streaming: failed enqueuing buffered body fd # {} errno={} msg={}", _fd, errno,
                     std::strerror(errno));
          return;
        }
      }
      _preCompressBuffer.clear();
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
  log::debug("Streaming: end fd # {} bytesWritten={} chunked={}", _fd, _bytesWritten, chunked());
}

bool HttpResponseWriter::enqueue(HttpResponseData httpResponseData) {
  // Access the connection state to determine backpressure / closure.
  auto cnxIt = _server->_connections.active.find(_fd);
  if (cnxIt == _server->_connections.active.end()) {
    return false;
  }
  return _server->queueData(cnxIt, std::move(httpResponseData)) && !cnxIt->second->isAnyCloseRequested();
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
  _preCompressBuffer.append(data);
  if (_preCompressBuffer.size() < compressionConfig.minBytes) {
    // Still below threshold; do not emit headers/body yet.
    return true;
  }
  // Threshold reached exactly or exceeded: activate encoder.
  _activeEncoderCtx = _server->_compression.makeContext(_compressionFormat);
  _compressionActivated = true;
  // Set Content-Encoding prior to emitting headers.
  // We can use headerAddLine instead of header because at this point the user has not set it.
  if (_state != HttpResponseWriter::State::HeadersSent) {
    _fixedResponse.headerAddLine(http::ContentEncoding, GetEncodingStr(_compressionFormat));
    if (_server->_config.compression.addVaryAcceptEncodingHeader) {
      _fixedResponse.headerAppendValue(http::Vary, http::AcceptEncoding);
    }
  }
  ensureHeadersSent();
  // Compress buffered bytes.
  auto firstOut = _activeEncoderCtx->encodeChunk(_preCompressBuffer);
  _preCompressBuffer.clear();
  if (!firstOut.empty()) {
    if (chunked()) {
      emitChunk(firstOut);
    } else {
      return enqueue(HttpResponseData(firstOut));
    }
  }
  return true;
}

}  // namespace aeronet
