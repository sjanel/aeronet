#include "aeronet/http-response-writer.hpp"

#include <cassert>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <memory>  // std::make_unique
#include <string>  // std::to_string
#include <string_view>
#include <system_error>
#include <utility>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-server.hpp"
#include "encoder.hpp"
#include "http-constants.hpp"
#include "http-status-code.hpp"
#include "http-version.hpp"  // http::HTTP_1_1 version constant
#include "log.hpp"
#include "static-string-view-helpers.hpp"
#include "string-equal-ignore-case.hpp"

namespace aeronet {

HttpResponseWriter::HttpResponseWriter(HttpServer& srv, int fd, bool headRequest, bool requestConnClose,
                                       Encoding compressionFormat)
    : _server(&srv),
      _fd(fd),
      _head(headRequest),
      _requestConnClose(requestConnClose),
      _compressionFormat(compressionFormat),
      _activeEncoderCtx(std::make_unique<IdentityEncoderContext>()) {}

void HttpResponseWriter::statusCode(http::StatusCode code, std::string_view reason) {
  if (_headersSent || _failed) {
    log::debug("Streaming: statusCode ignored fd={} reason={}", _fd,
               _failed ? "writer-failed" : "headers-already-sent");
    return;
  }
  _fixedResponse.statusCode(code).reason(reason);
}

void HttpResponseWriter::statusCode(http::StatusCode code) { statusCode(code, http::ReasonOK); }

void HttpResponseWriter::customHeader(std::string_view name, std::string_view value) {
  if (_headersSent || _failed) {
    log::debug("Streaming: header ignored fd={} name={} reason={}", _fd, name,
               _failed ? "writer-failed" : "headers-already-sent");
    return;
  }
  if (HttpResponse::IsReservedHeader(name)) {
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
  // Use 'header' to enforce uniqueness semantics for streaming path.
  _fixedResponse.customHeader(name, value);
}

void HttpResponseWriter::contentLength(std::size_t len) {
  if (_headersSent || _bytesWritten > 0 || _failed) {
    const char* reason;
    if (_failed) {
      reason = "writer-failed";
    } else if (_headersSent) {
      reason = "headers-already-sent";
    } else if (_bytesWritten > 0) {
      reason = "body-bytes-already-written";
    } else {
      reason = "unknown";
    }
    log::debug("Streaming: contentLength ignored fd={} requestedLen={} reason={}", _fd, len, reason);
    return;
  }
  _chunked = false;
  _declaredLength = len;
  _fixedResponse.setHeader(http::ContentLength, std::to_string(len));
}

void HttpResponseWriter::ensureHeadersSent() {
  if (_headersSent || _failed) {
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
  auto dateStr =
      std::string_view(reinterpret_cast<const char*>(_server->_cachedDate.data()), _server->_cachedDate.size());
  auto finalized = _fixedResponse.finalizeAndGetFullTextResponse(http::HTTP_1_1, dateStr, !_requestConnClose, _head);
  log::debug("Streaming: headers fd={} code={} chunked={} headerBytes={} ", _fd, _fixedResponse.statusCode(), _chunked,
             finalized.size());
  if (!enqueue(finalized)) {
    _failed = true;
    _ended = true;
    log::error("Streaming: failed to enqueue headers fd={} errno={} msg={}", _fd, errno, std::strerror(errno));
    return;
  }
  _headersSent = true;
}

void HttpResponseWriter::emitChunk(std::string_view data) {
  if (_head || data.empty() || _failed) {
    return;
  }

  char sizeLine[32];  // hex length + CRLF
  char* begin = sizeLine;
  char* end = sizeLine + sizeof(sizeLine) - http::CRLF.size();
  auto res = std::to_chars(begin, end, static_cast<unsigned long long>(data.size()), 16);
  assert(res.ec == std::errc());
  std::memcpy(res.ptr, http::CRLF.data(), http::CRLF.size());
  res.ptr += http::CRLF.size();
  std::size_t sizeHeaderLen = static_cast<std::size_t>(res.ptr - sizeLine);

  // Adaptive strategy: coalesce small/medium chunks to a single enqueue; avoid copying for large payloads.
  static constexpr std::size_t kCoalesceThreshold = 4096;  // heuristic – tune with benchmarks
  if (data.size() <= kCoalesceThreshold) {
    _chunkBuf.clear();
    _chunkBuf.ensureAvailableCapacity(sizeHeaderLen + data.size() + http::CRLF.size());
    _chunkBuf.unchecked_append(sizeLine, sizeHeaderLen);
    _chunkBuf.unchecked_append(data);
    _chunkBuf.unchecked_append(http::CRLF);
    if (!enqueue(_chunkBuf)) {
      _failed = true;
      _ended = true;
      log::error("Streaming: failed enqueuing coalesced chunk fd={} errno={} msg={}", _fd, errno, std::strerror(errno));
      return;
    }
    ++_server->_stats.streamingChunkCoalesced;
  } else {
    // For large payloads, we call 3 times enqueue to avoid copy on temporary data first.
    std::string_view sizeHeader(sizeLine, sizeHeaderLen);
    if (!enqueue(sizeHeader)) {
      _failed = true;
      _ended = true;
      log::error("Streaming: failed enqueuing chunk size header fd={} errno={} msg={}", _fd, errno,
                 std::strerror(errno));
      return;
    }
    if (!enqueue(data)) {
      _failed = true;
      _ended = true;
      log::error("Streaming: failed enqueuing chunk data fd={} errno={} msg={}", _fd, errno, std::strerror(errno));
      return;
    }
    if (!enqueue(http::CRLF)) {
      _failed = true;
      _ended = true;
      log::error("Streaming: failed enqueuing chunk trailer CRLF fd={} errno={} msg={}", _fd, errno,
                 std::strerror(errno));
      return;
    }
    ++_server->_stats.streamingChunkLarge;
  }
  _bytesWritten += data.size();
}

void HttpResponseWriter::emitLastChunk() {
  if (!_chunked || _head || _failed) {
    return;
  }
  static constexpr std::string_view last = JoinStringView_v<CharToStringView_v<'0'>, http::DoubleCRLF>;
  if (!enqueue(last)) {
    _failed = true;
    log::error("Streaming: failed enqueuing last chunk fd={} errno={} msg={}", _fd, errno, std::strerror(errno));
  }
}

bool HttpResponseWriter::write(std::string_view data) {
  if (_ended || _failed) {
    log::debug("Streaming: write ignored fd={} size={} reason={}", _fd, data.size(),
               _failed ? "writer-failed" : "already-ended");
    return false;
  }
  // Threshold-based lazy activation using generic Encoder abstraction.
  // We purposefully delay header emission until we either (a) activate compression and have compressed bytes
  // to send or (b) decide to emit identity data (on end()). This allows us to include the Content-Encoding header
  // reliably when compression triggers mid-stream.
  if (_compressionFormat != Encoding::none && !_userProvidedContentEncoding && !_compressionActivated) {
    const auto& compressionConfig = _server->_config.compression;
    // Accumulate data into the pre-compression buffer up to minBytes. Always buffer the entire incoming data until
    // we cross the threshold (or end() is called).
    if (_preCompressBuffer.size() < compressionConfig.minBytes) {
      _preCompressBuffer.append(data.begin(), data.end());
      if (_preCompressBuffer.size() < compressionConfig.minBytes) {
        // Still below threshold; do not emit headers/body yet.
        return !_failed;
      }
      // Threshold reached exactly or exceeded: activate encoder.
      auto& proto = _server->_encoders[static_cast<std::size_t>(_compressionFormat)];
      if (proto) {
        _activeEncoderCtx = proto->makeContext();
        _compressionActivated = true;
      } else {
        // Fallback silently to identity.
        _compressionFormat = Encoding::none;
      }
      if (_compressionActivated) {
        // Set Content-Encoding prior to emitting headers.
        if (!_headersSent) {
          _fixedResponse.setHeader(http::ContentEncoding, GetEncodingStr(_compressionFormat));
          if (_server->_config.compression.addVaryHeader) {
            _fixedResponse.setHeader(http::Vary, http::AcceptEncoding);
          }
        }
        ensureHeadersSent();
        // Compress buffered bytes.
        auto firstOut = _activeEncoderCtx->encodeChunk(
            std::string_view{_preCompressBuffer.data(), _preCompressBuffer.size()}, false);
        if (!firstOut.empty()) {
          if (_chunked) {
            emitChunk(firstOut);
          } else {
            enqueue(firstOut);
          }
        }
        _preCompressBuffer.clear();
        return !_failed;
      }
      // If we fell back to identity compressionFormat::none we continue below emitting identity data.
    }
  }

  // Compression already active OR disabled: emit data (compressed) immediately.
  if (_compressionActivated) {
    ensureHeadersSent();
    auto out = _activeEncoderCtx->encodeChunk(data, false);
    if (!out.empty()) {
      if (_chunked) {
        emitChunk(out);
      } else {
        enqueue(out);
      }
    }
    return !_failed;
  }

  // Identity path (either compression disabled or not yet activated and we choose to stream directly):
  ensureHeadersSent();
  if (_chunked) {
    emitChunk(_activeEncoderCtx->encodeChunk(data, false));
  } else if (!_head && !data.empty()) {
    if (!enqueue(data)) {
      _failed = true;
      _ended = true;
      log::error("Streaming: failed enqueuing fixed body fd={} errno={} msg={}", _fd, errno, std::strerror(errno));
      return false;
    }
    _bytesWritten += data.size();
  }
  log::trace("Streaming: write fd={} size={} total={} chunked={}", _fd, data.size(), _bytesWritten, _chunked);
  return !_failed;  // backpressure signaled via server.shouldClose flag, failure sets _failed
}

void HttpResponseWriter::end() {
  if (_ended || _failed) {
    log::debug("Streaming: end ignored fd={} reason={}", _fd, _failed ? "writer-failed" : "already-ended");
    return;
  }
  // If compression was delayed and threshold reached earlier, write() already emitted headers and compressed data.
  // Otherwise we may still have buffered identity bytes (below threshold case) — emit headers now then flush.
  if (_compressionActivated) {
    ensureHeadersSent();
    auto last = _activeEncoderCtx->encodeChunk({}, true);
    if (!last.empty()) {
      if (_chunked) {
        emitChunk(last);
      } else if (!_head) {
        enqueue(last);
      }
    }
  } else {
    // Identity path; emit headers now (they may not have been sent yet due to delayed strategy) then flush buffered.
    ensureHeadersSent();
    if (!_preCompressBuffer.empty()) {
      if (_chunked) {
        emitChunk(std::string_view{_preCompressBuffer.data(), _preCompressBuffer.size()});
      } else if (!_head) {
        enqueue(std::string_view{_preCompressBuffer.data(), _preCompressBuffer.size()});
      }
      _preCompressBuffer.clear();
    }
  }

  emitLastChunk();
  _ended = true;
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
  log::debug("Streaming: end fd={} bytesWritten={} chunked={}", _fd, _bytesWritten, _chunked);
}

bool HttpResponseWriter::enqueue(std::string_view data) {
  // All current call sites guarantee non-empty payloads. If this ever fires,
  // it indicates an internal logic error (e.g. attempting to enqueue an empty buffer
  // instead of short‑circuiting earlier at the call site).
  assert(!data.empty());
  // Access the connection state to determine backpressure / closure.
  auto it = _server->_connStates.find(_fd);
  if (it == _server->_connStates.end()) {
    return false;
  }
  auto& st = it->second;
  bool ok = _server->queueData(_fd, st, data);
  return ok && !st.shouldClose;
}

}  // namespace aeronet
