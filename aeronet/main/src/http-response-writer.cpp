#include "aeronet/http-response-writer.hpp"

#include <cassert>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <system_error>
#include <utility>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "encoder.hpp"
#include "header-write.hpp"
#include "log.hpp"
#include "raw-chars.hpp"
#include "string-equal-ignore-case.hpp"
#include "stringconv.hpp"
#include "timedef.hpp"

namespace aeronet {

HttpResponseWriter::HttpResponseWriter(HttpServer& srv, int fd, bool headRequest, bool requestConnClose,
                                       Encoding compressionFormat)
    : _server(&srv),
      _fd(fd),
      _head(headRequest),
      _requestConnClose(requestConnClose),
      _compressionFormat(compressionFormat),
      _activeEncoderCtx(std::make_unique<IdentityEncoderContext>()) {}

void HttpResponseWriter::statusCode(http::StatusCode code) {
  if (_state != State::Opened) {
    return;
  }
  _fixedResponse.statusCode(code);
}

void HttpResponseWriter::statusCode(http::StatusCode code, std::string_view reason) {
  if (_state != State::Opened) {
    return;
  }
  _fixedResponse.statusCode(code).reason(reason);
}

void HttpResponseWriter::addCustomHeader(std::string_view name, std::string_view value) {
  if (_state != State::Opened) {
    return;
  }
  if (http::IsReservedResponseHeader(name)) {
    log::error("Attempt to set reserved or managed header '{}' in streaming response", name);
    return;
  }
  if (CaseInsensitiveEqual(name, http::ContentType)) {
    // Track explicit user override of default content type.
    _userSetContentType = true;
  }
  if (CaseInsensitiveEqual(name, http::ContentEncoding)) {
    _userProvidedContentEncoding = true;  // suppress automatic compression
    // If user sets identity we still treat it as suppression; we do not validate value here.
  }
  _fixedResponse.addCustomHeader(name, value);
}

void HttpResponseWriter::customHeader(std::string_view name, std::string_view value) {
  if (_state != State::Opened) {
    log::debug("Streaming: header ignored fd # {} name={} reason={}", _fd, name,
               _state == State::Failed ? "writer-failed" : "headers-already-sent");
    return;
  }
  if (http::IsReservedResponseHeader(name)) {
    log::error("Attempt to set reserved or managed header '{}' in streaming response", name);
    return;
  }
  if (CaseInsensitiveEqual(name, http::ContentType)) {
    // Track explicit user override of default content type.
    _userSetContentType = true;
  }
  if (CaseInsensitiveEqual(name, http::ContentEncoding)) {
    _userProvidedContentEncoding = true;  // suppress automatic compression
    // If user sets identity we still treat it as suppression; we do not validate value here.
  }
  _fixedResponse.customHeader(name, value);
}

void HttpResponseWriter::contentLength(std::size_t len) {
  if (_state != State::Opened || _bytesWritten > 0) {
    std::string_view reason;
    if (_state == State::Failed) {
      reason = "writer-failed";
    } else if (_state == HttpResponseWriter::State::HeadersSent) {
      reason = "headers-already-sent";
    } else if (_bytesWritten > 0) {
      reason = "body-bytes-already-written";
    } else {
      reason = "unknown";
    }
    log::warn("Streaming: contentLength ignored fd # {} requestedLen={} reason={}", _fd, len, reason);
    return;
  }
  _chunked = false;
  _declaredLength = len;
  _fixedResponse.setHeader(http::ContentLength, std::string_view(IntegralToCharVector(len)));
}

void HttpResponseWriter::ensureHeadersSent() {
  if (_state != State::Opened) {
    return;
  }
  // For HEAD requests never emit chunked framing; force zero Content-Length if not provided.
  if (_head) {
    _chunked = false;
  }
  if (!_chunked) {
    // If user never called contentLength() we still provide an explicit length (zero) for fixed path
    if (_declaredLength == 0) {
      _fixedResponse.setHeader(http::ContentLength, "0");
    }
  } else {
    _fixedResponse.setHeader(http::TransferEncoding, "chunked");
  }
  if (!_userSetContentType) {
    _fixedResponse.setHeader(http::ContentType, http::ContentTypeTextPlain);
  }
  // If compression already activated (delayed strategy) but header not sent yet, add Content-Encoding now.
  if (_compressionActivated && _compressionFormat != Encoding::none) {
    _fixedResponse.setHeader(http::ContentEncoding, GetEncodingStr(_compressionFormat));
    if (_server->_config.compression.addVaryHeader) {
      _fixedResponse.setHeader(http::Vary, http::AcceptEncoding);
    }
  }
  // Do NOT add Content-Encoding at header emission time; we wait until we actually activate
  // compression (threshold reached) to avoid mislabeling identity bodies when size < threshold.
  // Do not attempt to add Connection/Date here; finalize handles them (adds Date, Connection based on keepAlive flag).
  if (!enqueue(_fixedResponse.finalizeAndStealData(http::HTTP_1_1, SysClock::now(), !_requestConnClose,
                                                   _server->config().globalHeaders, _head,
                                                   _server->config().minCapturedBodySize))) {
    _state = HttpResponseWriter::State::Failed;
    log::error("Streaming: failed to enqueue headers fd # {} errno={} msg={}", _fd, errno, std::strerror(errno));
    return;
  }
  _state = HttpResponseWriter::State::HeadersSent;
}

void HttpResponseWriter::emitChunk(std::string_view data) {
  if (_head || data.empty() || _state == State::Failed) {
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
  if (!enqueue(HttpResponseData(std::move(chunkBuf)))) {
    _state = HttpResponseWriter::State::Failed;
    log::error("Streaming: failed enqueuing coalesced chunk fd # {} errno={} msg={}", _fd, errno, std::strerror(errno));
    return;
  }
  _bytesWritten += data.size();
}

void HttpResponseWriter::emitLastChunk() {
  if (!_chunked || _head || _state == State::Failed) {
    return;
  }

  // Emit final chunk with optional trailers (RFC 7230 §4.1.2):
  //   0\r\n
  //   [trailer-name: value\r\n]*
  //   \r\n
  if (_trailers.empty()) {
    _trailers.ensureAvailableCapacity(1UL + http::DoubleCRLF.size());
    _trailers.unchecked_push_back('0');
    _trailers.unchecked_append(http::CRLF);
  }
  _trailers.unchecked_append(http::CRLF);  // Final blank line (memory already reserved)

  if (!enqueue(HttpResponseData(std::move(_trailers)))) {
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

  // Threshold-based lazy activation using generic Encoder abstraction.
  // We purposefully delay header emission until we either (a) activate compression and have compressed bytes
  // to send or (b) decide to emit identity data (on end()). This allows us to include the Content-Encoding header
  // reliably when compression triggers mid-stream.
  const auto& compressionConfig = _server->_config.compression;
  if (_compressionFormat != Encoding::none && !_userProvidedContentEncoding && !_compressionActivated &&
      _preCompressBuffer.size() < compressionConfig.minBytes) {
    return accumulateInPreCompressBuffer(data);
  }

  ensureHeadersSent();

  data = _activeEncoderCtx->encodeChunk(compressionConfig.encoderChunkSize, data, false);

  if (_chunked) {
    emitChunk(data);
  } else if (!_head) {
    if (!enqueue(HttpResponseData(data))) {
      _state = HttpResponseWriter::State::Failed;
      log::error("Streaming: failed enqueuing fixed body fd # {} errno={} msg={}", _fd, errno, std::strerror(errno));
      return false;
    }
    _bytesWritten += data.size();
  }
  log::trace("Streaming: write fd # {} size={} total={} chunked={}", _fd, data.size(), _bytesWritten, _chunked);
  return _state != State::Failed;  // backpressure signaled via connection close flag, failure sets failed state
}

void HttpResponseWriter::addTrailer(std::string_view name, std::string_view value) {
  if (_state == State::Ended || _state == State::Failed) {
    log::warn("Streaming: addTrailer ignored fd # {} name={} reason={}", _fd, name,
              _state == State::Failed ? "writer-failed" : "already-ended");
    return;
  }
  if (!_chunked) {
    log::warn("Streaming: addTrailer ignored fd # {} name={} reason=fixed-length-response (contentLength was set)", _fd,
              name);
    return;
  }

  // Trailer format: name ": " value CRLF
  const std::size_t lineSize = name.size() + http::HeaderSep.size() + value.size() + http::CRLF.size();

  if (_trailers.empty()) {
    _trailers.ensureAvailableCapacity(lineSize + 1UL + http::DoubleCRLF.size());
    _trailers.unchecked_push_back('0');
    _trailers.unchecked_append(http::CRLF);
  } else {
    _trailers.ensureAvailableCapacity(lineSize);
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
  // If compression was delayed and threshold reached earlier, write() already emitted headers and compressed data.
  // Otherwise we may still have buffered identity bytes (below threshold case) — emit headers now then flush.
  ensureHeadersSent();
  if (_compressionActivated) {
    const auto& compressionConfig = _server->_config.compression;
    auto last = _activeEncoderCtx->encodeChunk(compressionConfig.encoderChunkSize, {}, true);
    if (!last.empty()) {
      if (_chunked) {
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
      if (_chunked) {
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
  if (!_chunked && !_head) {
    // _declaredLength may be zero either because user explicitly set it or because we synthesized 0 for HEAD;
    // for HEAD we suppress body so skip. For identity path we track bytesWritten; for compression path we cannot
    // validate because encoder output size may differ from raw input; in that case we only asserted the user should
    // not declare a length unless they know the final encoded size. We still assert if compression was never
    // activated (identity or user-supplied encoding) that counts match.
    if (!_compressionActivated || _compressionFormat == Encoding::none || _userProvidedContentEncoding) {
      assert(_bytesWritten == _declaredLength && "Declared Content-Length does not match bytes written");
    }
  }
#endif
  log::debug("Streaming: end fd # {} bytesWritten={} chunked={}", _fd, _bytesWritten, _chunked);
}

bool HttpResponseWriter::enqueue(HttpResponseData httpResponseData) {
  // Access the connection state to determine backpressure / closure.
  auto cnxIt = _server->_connStates.find(_fd);
  if (cnxIt == _server->_connStates.end()) {
    return false;
  }
  return _server->queueData(cnxIt, std::move(httpResponseData)) && !cnxIt->second.isAnyCloseRequested();
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
  auto& encoderPtr = _server->_encoders[static_cast<std::size_t>(_compressionFormat)];
  _activeEncoderCtx = encoderPtr->makeContext();
  _compressionActivated = true;
  // Set Content-Encoding prior to emitting headers.
  if (_state != HttpResponseWriter::State::HeadersSent) {
    _fixedResponse.setHeader(http::ContentEncoding, GetEncodingStr(_compressionFormat));
    if (_server->_config.compression.addVaryHeader) {
      _fixedResponse.setHeader(http::Vary, http::AcceptEncoding);
    }
  }
  ensureHeadersSent();
  // Compress buffered bytes.
  auto firstOut = _activeEncoderCtx->encodeChunk(compressionConfig.encoderChunkSize, _preCompressBuffer, false);
  _preCompressBuffer.clear();
  if (!firstOut.empty()) {
    if (_chunked) {
      emitChunk(firstOut);
    } else {
      return enqueue(HttpResponseData(firstOut));
    }
  }
  return true;
}

}  // namespace aeronet
