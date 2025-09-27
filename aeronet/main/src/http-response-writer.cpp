#include "aeronet/http-response-writer.hpp"

#include <cassert>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "aeronet/http-server.hpp"
#include "http-constants.hpp"
#include "http-status-build.hpp"
#include "http-status-code.hpp"
#include "log.hpp"
#include "raw-chars.hpp"

namespace aeronet {

namespace {
constexpr std::string kContentLength = "Content-Length";
constexpr std::string kContentType = "Content-Type";
}  // namespace

HttpResponseWriter::HttpResponseWriter(HttpServer& srv, int fd, bool headRequest, bool requestConnClose)
    : _server(&srv), _fd(fd), _head(headRequest), _requestConnClose(requestConnClose) {}

void HttpResponseWriter::statusCode(http::StatusCode code, std::string reason) {
  if (_headersSent || _failed) {
    return;
  }
  _statusCode = code;
  _reason = std::move(reason);
}

void HttpResponseWriter::header(std::string name, std::string value) {
  if (_headersSent || _failed) {
    return;
  }
  // Reuse HttpResponse reservation policy for symmetry; allow user to set Content-Length only via setContentLength.
  if (HttpResponse::IsReservedHeader(name)) {
    assert(false && "Attempt to set reserved or managed header in streaming response");
    return;  // ignore in release builds
  }
  _headers.insert_or_assign(std::move(name), std::move(value));
}

void HttpResponseWriter::contentLength(std::size_t len) {
  if (_headersSent || _bytesWritten > 0 || _failed) {
    return;
  }
  _chunked = false;
  _declaredLength = len;
  _headers[kContentLength] = std::to_string(len);
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
    if (_headers.find(kContentLength) == _headers.end()) {
      _headers[kContentLength] = "0";
    }
  } else {
    _headers[std::string(http::TransferEncoding)] = "chunked";
  }
  // Connection header: if client asked to close, force it (can't be overridden by user since reserved).
  if (_requestConnClose) {
    _headers[std::string(http::Connection)] = std::string(http::close);
  }
  if (_headers.find(kContentType) == _headers.end()) {
    _headers[kContentType] = std::string(http::ContentTypeTextPlain);
  }
  auto head = http::buildStatusLine(_statusCode, _reason);
  for (auto& kv : _headers) {
    head.append(kv.first);
    head.append(": ");
    head.append(kv.second);
    head.append(http::CRLF);
  }
  head.append(http::CRLF);
  log::debug("Streaming: headers fd={} code={} chunked={} headerCount={}", _fd, _statusCode, _chunked, _headers.size());
  if (!enqueue(head)) {
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
  // Build entire chunk (size line + data + CRLF) contiguously to increase chance of single send
  char sizeLine[32];  // enough for 16 hex digits + CRLF
  char* begin = sizeLine;
  char* end = sizeLine + sizeof(sizeLine) - 2;  // leave space for CRLF
  auto res = std::to_chars(begin, end, static_cast<unsigned long long>(data.size()), 16);
  assert(res.ec == std::errc());
  *res.ptr++ = '\r';
  *res.ptr++ = '\n';
  size_t sizeHeaderLen = static_cast<size_t>(res.ptr - sizeLine);
  RawChars chunk(sizeHeaderLen + data.size() + 2);
  chunk.unchecked_append(sizeLine, sizeHeaderLen);
  chunk.unchecked_append(data);
  chunk.unchecked_append("\r\n", 2);
  if (!enqueue(chunk)) {
    _failed = true;
    _ended = true;
    log::error("Streaming: failed enqueuing coalesced chunk fd={} errno={} msg={}", _fd, errno, std::strerror(errno));
    return;
  }
  _bytesWritten += data.size();
}

void HttpResponseWriter::emitLastChunk() {
  if (!_chunked || _head || _failed) {
    return;
  }
  static constexpr std::string_view last = "0\r\n\r\n";
  if (!enqueue(last)) {
    _failed = true;
    log::error("Streaming: failed enqueuing last chunk fd={} errno={} msg={}", _fd, errno, std::strerror(errno));
  }
}

bool HttpResponseWriter::write(std::string_view data) {
  if (_ended || _failed) {
    return false;
  }
  ensureHeadersSent();
  if (_chunked) {
    emitChunk(data);
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
    return;
  }
  ensureHeadersSent();
  emitLastChunk();
  _ended = true;
  log::debug("Streaming: end fd={} bytesWritten={} chunked={}", _fd, _bytesWritten, _chunked);
}

bool HttpResponseWriter::enqueue(std::string_view data) {
  if (data.empty()) {
    return true;
  }
  if (_server == nullptr) {
    return false;
  }
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
