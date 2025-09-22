#include <algorithm>
#include <cstring>

#include "aeronet/server.hpp"
#include "http-constants.hpp"
#include "http-error-build.hpp"
#include "raw-chars.hpp"

namespace aeronet {

bool HttpServer::parseNextRequestFromBuffer(int fd, ConnStateInternal& state, HttpRequest& outReq,
                                            std::size_t& headerEnd, bool& closeConn) {
  static constexpr std::string_view kDoubleCRLF = "\r\n\r\n";
  auto rng = std::ranges::search(state.buffer, kDoubleCRLF);
  if (rng.empty()) {
    return false;  // need more bytes
  }
  headerEnd = rng.begin() - state.buffer.data();
  if (headerEnd > _config.maxHeaderBytes) {
    auto err = buildSimpleError(431, http::ReasonHeadersTooLarge, std::string_view(_cachedDate), true);
    queueData(fd, state, err.data(), err.size());
    if (_parserErrCb) {
      _parserErrCb(ParserError::HeadersTooLarge);
    }
    closeConn = true;
    return false;
  }
  const char* begin = state.buffer.data();
  const char* headers_end_ptr = begin + headerEnd;
  const char* raw_line_nl = static_cast<const char*>(std::memchr(begin, '\n', static_cast<size_t>(headerEnd)));
  if (raw_line_nl == nullptr) {
    auto err = buildSimpleError(400, http::ReasonBadRequest, std::string_view(_cachedDate), true);
    queueData(fd, state, err.data(), err.size());
    if (_parserErrCb) {
      _parserErrCb(ParserError::BadRequestLine);
    }
    closeConn = true;
    return false;
  }
  const char* line_start = begin;
  const char* line_end_trim = raw_line_nl;
  if (line_end_trim > line_start && *(line_end_trim - 1) == '\r') {
    --line_end_trim;
  }
  const char* sp1 =
      static_cast<const char*>(std::memchr(line_start, ' ', static_cast<size_t>(line_end_trim - line_start)));
  if (sp1 == nullptr) {
    auto err = buildSimpleError(400, http::ReasonBadRequest, std::string_view(_cachedDate), true);
    queueData(fd, state, err.data(), err.size());
    if (_parserErrCb) {
      _parserErrCb(ParserError::BadRequestLine);
    }
    closeConn = true;
    return false;
  }
  const char* sp2 = static_cast<const char*>(std::memchr(sp1 + 1, ' ', static_cast<size_t>(line_end_trim - (sp1 + 1))));
  if (sp2 == nullptr) {
    auto err = buildSimpleError(400, http::ReasonBadRequest, std::string_view(_cachedDate), true);
    queueData(fd, state, err.data(), err.size());
    if (_parserErrCb) {
      _parserErrCb(ParserError::BadRequestLine);
    }
    closeConn = true;
    return false;
  }
  outReq.method = {line_start, static_cast<size_t>(sp1 - line_start)};
  outReq.target = {sp1 + 1, static_cast<size_t>(sp2 - (sp1 + 1))};
  outReq.version = {sp2 + 1, static_cast<size_t>(line_end_trim - (sp2 + 1))};
  if (!(outReq.version == http::HTTP11 || outReq.version == http::HTTP10)) {
    auto err = buildSimpleError(505, http::ReasonHTTPVersionNotSupported, std::string_view(_cachedDate), true);
    queueData(fd, state, err.data(), err.size());
    if (_parserErrCb) {
      _parserErrCb(ParserError::VersionUnsupported);
    }
    closeConn = true;
    return false;
  }
  const char* cursor = (raw_line_nl < headers_end_ptr) ? (raw_line_nl + 1) : headers_end_ptr;
  while (cursor < headers_end_ptr) {
    const char* next_nl =
        static_cast<const char*>(std::memchr(cursor, '\n', static_cast<size_t>(headers_end_ptr - cursor)));
    if (next_nl == nullptr) {
      next_nl = headers_end_ptr;
    }
    const char* line_e = next_nl;
    if (line_e > cursor && *(line_e - 1) == '\r') {
      --line_e;
    }
    if (line_e == cursor) {
      break;
    }
    const char* colon = static_cast<const char*>(std::memchr(cursor, ':', static_cast<size_t>(line_e - cursor)));
    if (colon != nullptr) {
      const char* value_beg = colon + 1;
      while (value_beg < line_e && (*value_beg == ' ' || *value_beg == '\t')) {
        ++value_beg;
      }
      const char* value_end = line_e;
      while (value_end > value_beg && (*(value_end - 1) == ' ' || *(value_end - 1) == '\t')) {
        --value_end;
      }
      outReq.headers.emplace(std::make_pair(std::string_view(cursor, colon), std::string_view(value_beg, value_end)));
    }
    cursor = (next_nl < headers_end_ptr) ? (next_nl + 1) : headers_end_ptr;
  }
  return true;
}

bool HttpServer::decodeBodyIfReady(int fd, ConnStateInternal& state, const HttpRequest& req, std::size_t headerEnd,
                                   bool isChunked, bool expectContinue, bool& closeConn, size_t& consumedBytes) {
  consumedBytes = 0;
  if (!isChunked) {
    return decodeFixedLengthBody(fd, state, req, headerEnd, expectContinue, closeConn, consumedBytes);
  }
  return decodeChunkedBody(fd, state, req, headerEnd, expectContinue, closeConn, consumedBytes);
}

bool HttpServer::decodeFixedLengthBody(int fd, ConnStateInternal& state, const HttpRequest& req, std::size_t headerEnd,
                                       bool expectContinue, bool& closeConn, size_t& consumedBytes) {
  std::string_view lenViewAll = req.findHeader(http::ContentLength);
  bool hasCL = !lenViewAll.empty();
  size_t contentLen = 0;
  if (!hasCL) {
    // No Content-Length and not chunked: treat as no body (common for GET/HEAD). Ready immediately.
    if (state.buffer.size() >= headerEnd + 4) {
      const_cast<HttpRequest&>(req).body = std::string_view{};
      consumedBytes = headerEnd + 4;
      return true;
    }
    return false;
  }
  if (hasCL) {
    size_t parsed = 0;
    for (char ch : lenViewAll) {
      if (ch < '0' || ch > '9') {
        parsed = _config.maxBodyBytes + 1;
        break;
      }
      parsed = (parsed * 10U) + static_cast<size_t>(ch - '0');
      if (parsed > _config.maxBodyBytes) {
        break;
      }
    }
    contentLen = parsed;
    if (contentLen > _config.maxBodyBytes) {
      auto err = buildSimpleError(413, http::ReasonPayloadTooLarge, std::string_view(_cachedDate), true);
      queueData(fd, state, err.data(), err.size());
      closeConn = true;
      return false;
    }
    if (expectContinue && contentLen > 0) {
      queueData(fd, state, http::HTTP11_100_CONTINUE.data(), http::HTTP11_100_CONTINUE.size());
    }
  }
  size_t totalNeeded = headerEnd + 4 + contentLen;
  if (state.buffer.size() < totalNeeded) {
    return false;  // need more bytes
  }
  const char* body_start = state.buffer.data() + headerEnd + 4;
  const_cast<HttpRequest&>(req).body = {body_start, contentLen};
  consumedBytes = totalNeeded;
  return true;
}

bool HttpServer::decodeChunkedBody(int fd, ConnStateInternal& state, const HttpRequest& req, std::size_t headerEnd,
                                   bool expectContinue, bool& closeConn, size_t& consumedBytes) {
  if (expectContinue) {
    queueData(fd, state, http::HTTP11_100_CONTINUE.data(), http::HTTP11_100_CONTINUE.size());
  }
  size_t pos = headerEnd + 4;
  RawChars decodedBody(1024);
  bool needMore = false;
  while (true) {
    auto lineEndIt = std::search(state.buffer.begin() + pos, state.buffer.end(), http::CRLF.begin(), http::CRLF.end());
    if (lineEndIt == state.buffer.end()) {
      needMore = true;
      break;
    }
    std::string_view sizeLine(state.buffer.data() + pos, lineEndIt);
    size_t chunkSize = 0;
    for (char ch : sizeLine) {
      if (ch == ';') {
        break;
      }
      unsigned value = 0;
      if (ch >= '0' && ch <= '9') {
        value = static_cast<unsigned>(ch - '0');
      } else if (ch >= 'a' && ch <= 'f') {
        value = static_cast<unsigned>(10 + (ch - 'a'));
      } else if (ch >= 'A' && ch <= 'F') {
        value = static_cast<unsigned>(10 + (ch - 'A'));
      } else {
        chunkSize = _config.maxBodyBytes + 1;
        break;
      }
      chunkSize = (chunkSize << 4) | static_cast<size_t>(value);
      if (chunkSize > _config.maxBodyBytes) {
        break;
      }
    }
    pos = (lineEndIt - state.buffer.data()) + 2;
    if (chunkSize > _config.maxBodyBytes) {
      auto err = buildSimpleError(413, http::ReasonPayloadTooLarge, std::string_view(_cachedDate), true);
      queueData(fd, state, err.data(), err.size());
      if (_parserErrCb) {
        _parserErrCb(ParserError::PayloadTooLarge);
      }
      closeConn = true;
      return false;
    }
    if (state.buffer.size() < pos + chunkSize + 2) {
      needMore = true;
      break;
    }
    if (chunkSize == 0) {
      if (state.buffer.size() < pos + 2) {
        needMore = true;
        break;
      }
      if (std::memcmp(state.buffer.data() + pos, http::CRLF.data(), http::CRLF.size()) != 0) {
        needMore = true;
        break;
      }
      pos += 2;
      break;
    }
    decodedBody.append(state.buffer.data() + pos, std::min(chunkSize, state.buffer.size() - pos));
    if (decodedBody.size() > _config.maxBodyBytes) {
      auto err = buildSimpleError(413, http::ReasonPayloadTooLarge, std::string_view(_cachedDate), true);
      queueData(fd, state, err.data(), err.size());
      if (_parserErrCb) {
        _parserErrCb(ParserError::PayloadTooLarge);
      }
      closeConn = true;
      return false;
    }
    pos += chunkSize;
    if (std::memcmp(state.buffer.data() + pos, http::CRLF.data(), http::CRLF.size()) != 0) {
      needMore = true;
      break;
    }
    pos += http::CRLF.size();
  }
  if (needMore) {
    return false;
  }
  state.bodyStorage.assign(decodedBody.data(), decodedBody.size());
  const_cast<HttpRequest&>(req).body = std::string_view(state.bodyStorage);
  consumedBytes = pos;
  return true;
}

}  // namespace aeronet
