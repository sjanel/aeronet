#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-server.hpp"
#include "char-hexadecimal-converter.hpp"
#include "http-constants.hpp"
#include "raw-chars.hpp"
#include "url-decode.hpp"

namespace aeronet {

bool HttpServer::parseNextRequestFromBuffer(int fd, ConnectionState& state, HttpRequest& outReq, std::size_t& headerEnd,
                                            bool& closeConn) {
  static constexpr std::string_view kDoubleCRLF = "\r\n\r\n";
  auto rng = std::ranges::search(state.buffer, kDoubleCRLF);
  if (rng.empty()) {
    return false;  // need more bytes
  }
  headerEnd = static_cast<std::size_t>(rng.begin() - state.buffer.data());
  if (headerEnd > _config.maxHeaderBytes) {
    return emitSimpleError(fd, state, 431, http::ReasonHeadersTooLarge, ParserError::HeadersTooLarge, closeConn);
  }
  const char* begin = state.buffer.data();
  const char* headersEndPtr = begin + headerEnd;
  const char* rawLineNl = static_cast<const char*>(std::memchr(begin, '\n', headerEnd));
  if (rawLineNl == nullptr) {
    return emitSimpleError(fd, state, 400, http::ReasonBadRequest, ParserError::BadRequestLine, closeConn);
  }
  const char* lineStart = begin;
  const char* lineEndTrim = rawLineNl;
  if (lineEndTrim > lineStart && *(lineEndTrim - 1) == '\r') {
    --lineEndTrim;
  }
  const char* sp1 =
      static_cast<const char*>(std::memchr(lineStart, ' ', static_cast<std::size_t>(lineEndTrim - lineStart)));
  if (sp1 == nullptr) {
    return emitSimpleError(fd, state, 400, http::ReasonBadRequest, ParserError::BadRequestLine, closeConn);
  }
  const char* sp2 =
      static_cast<const char*>(std::memchr(sp1 + 1, ' ', static_cast<std::size_t>(lineEndTrim - (sp1 + 1))));
  if (sp2 == nullptr) {
    return emitSimpleError(fd, state, 400, http::ReasonBadRequest, ParserError::BadRequestLine, closeConn);
  }
  outReq.method = {lineStart, static_cast<std::size_t>(sp1 - lineStart)};
  outReq.target = {sp1 + 1, static_cast<std::size_t>(sp2 - (sp1 + 1))};
  // Percent-decode the target (path + optional query) per RFC 3986 using per-connection buffer.
  // We do NOT treat '+' as space at the HTTP parsing layer.
  if (std::memchr(outReq.target.data(), '%', outReq.target.size()) != nullptr) {
    state.decodedTarget.assign(outReq.target.data(), outReq.target.size());
    if (!URLDecodeInPlace(state.decodedTarget, false)) {
      return emitSimpleError(fd, state, 400, http::ReasonBadRequest, ParserError::BadRequestLine, closeConn);
    }
    outReq.target = state.decodedTarget;
  }
  outReq.version = {sp2 + 1, static_cast<std::size_t>(lineEndTrim - (sp2 + 1))};
  if (!(outReq.version == http::HTTP11 || outReq.version == http::HTTP10)) {
    return emitSimpleError(fd, state, 505, http::ReasonHTTPVersionNotSupported, ParserError::VersionUnsupported,
                           closeConn);
  }
  const char* cursor = (rawLineNl < headersEndPtr) ? (rawLineNl + 1) : headersEndPtr;
  while (cursor < headersEndPtr) {
    const char* nextNl =
        static_cast<const char*>(std::memchr(cursor, '\n', static_cast<std::size_t>(headersEndPtr - cursor)));
    if (nextNl == nullptr) {
      nextNl = headersEndPtr;
    }
    const char* lineEnd = nextNl;
    if (lineEnd > cursor && *(lineEnd - 1) == '\r') {
      --lineEnd;
    }
    if (lineEnd == cursor) {
      break;
    }
    const char* colon = static_cast<const char*>(std::memchr(cursor, ':', static_cast<std::size_t>(lineEnd - cursor)));
    if (colon != nullptr) {
      const char* valueBeg = colon + 1;
      while (valueBeg < lineEnd && (*valueBeg == ' ' || *valueBeg == '\t')) {
        ++valueBeg;
      }
      const char* valueEnd = lineEnd;
      while (valueEnd > valueBeg && (*(valueEnd - 1) == ' ' || *(valueEnd - 1) == '\t')) {
        --valueEnd;
      }
      outReq.headers.emplace(std::make_pair(std::string_view(cursor, colon), std::string_view(valueBeg, valueEnd)));
    }
    cursor = (nextNl < headersEndPtr) ? (nextNl + 1) : headersEndPtr;
  }
  return true;
}

bool HttpServer::decodeBodyIfReady(int fd, ConnectionState& state, const HttpRequest& req, std::size_t headerEnd,
                                   bool isChunked, bool expectContinue, bool& closeConn, std::size_t& consumedBytes) {
  consumedBytes = 0;
  if (!isChunked) {
    return decodeFixedLengthBody(fd, state, req, headerEnd, expectContinue, closeConn, consumedBytes);
  }
  return decodeChunkedBody(fd, state, req, headerEnd, expectContinue, closeConn, consumedBytes);
}

bool HttpServer::decodeFixedLengthBody(int fd, ConnectionState& state, const HttpRequest& req, std::size_t headerEnd,
                                       bool expectContinue, bool& closeConn, std::size_t& consumedBytes) {
  std::string_view lenViewAll = req.findHeader(http::ContentLength);
  bool hasCL = !lenViewAll.empty();
  std::size_t contentLen = 0;
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
    std::size_t parsed = 0;
    for (char ch : lenViewAll) {
      if (ch < '0' || ch > '9') {
        parsed = _config.maxBodyBytes + 1;
        break;
      }
      parsed = (parsed * 10U) + static_cast<std::size_t>(ch - '0');
      if (parsed > _config.maxBodyBytes) {
        break;
      }
    }
    contentLen = parsed;
    if (contentLen > _config.maxBodyBytes) {
      return emitSimpleError(fd, state, 413, http::ReasonPayloadTooLarge, ParserError::PayloadTooLarge, closeConn);
    }
    if (expectContinue && contentLen > 0) {
      queueData(fd, state, http::HTTP11_100_CONTINUE);
    }
  }
  std::size_t totalNeeded = headerEnd + 4 + contentLen;
  if (state.buffer.size() < totalNeeded) {
    return false;  // need more bytes
  }
  const char* bodyStart = state.buffer.data() + headerEnd + 4;
  const_cast<HttpRequest&>(req).body = {bodyStart, contentLen};
  consumedBytes = totalNeeded;
  return true;
}

bool HttpServer::decodeChunkedBody(int fd, ConnectionState& state, const HttpRequest& req, std::size_t headerEnd,
                                   bool expectContinue, bool& closeConn, std::size_t& consumedBytes) {
  if (expectContinue) {
    queueData(fd, state, http::HTTP11_100_CONTINUE);
  }
  std::size_t pos = headerEnd + 4U;
  RawChars decodedBody(1024);
  bool needMore = false;
  while (true) {
    auto lineEndIt = std::search(state.buffer.begin() + pos, state.buffer.end(), http::CRLF.begin(), http::CRLF.end());
    if (lineEndIt == state.buffer.end()) {
      needMore = true;
      break;
    }
    std::string_view sizeLine(state.buffer.data() + pos, lineEndIt);
    std::size_t chunkSize = 0;
    for (char ch : sizeLine) {
      if (ch == ';') {
        break;  // ignore chunk extensions per RFC 7230 section 4.1.1
      }
      int digit = from_hex_digit(ch);
      if (digit < 0) {
        chunkSize = _config.maxBodyBytes + 1;  // trigger payload too large / invalid
        break;
      }
      chunkSize = (chunkSize << 4) | static_cast<std::size_t>(digit);
      if (chunkSize > _config.maxBodyBytes) {
        break;
      }
    }
    pos = static_cast<std::size_t>(lineEndIt - state.buffer.data()) + 2;
    if (chunkSize > _config.maxBodyBytes) {
      return emitSimpleError(fd, state, 413, http::ReasonPayloadTooLarge, ParserError::PayloadTooLarge, closeConn);
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
      return emitSimpleError(fd, state, 413, http::ReasonPayloadTooLarge, ParserError::PayloadTooLarge, closeConn);
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
