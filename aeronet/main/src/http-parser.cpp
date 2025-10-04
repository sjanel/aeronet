#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-server.hpp"
#include "char-hexadecimal-converter.hpp"
#include "connection-state.hpp"
#include "raw-chars.hpp"

namespace aeronet {

bool HttpServer::decodeBodyIfReady(int fd, ConnectionState& state, HttpRequest& req, bool isChunked,
                                   bool expectContinue, std::size_t& consumedBytes) {
  consumedBytes = 0;
  if (isChunked) {
    return decodeChunkedBody(fd, state, req, expectContinue, consumedBytes);
  }
  return decodeFixedLengthBody(fd, state, req, expectContinue, consumedBytes);
}

bool HttpServer::decodeFixedLengthBody(int fd, ConnectionState& state, HttpRequest& req, bool expectContinue,
                                       std::size_t& consumedBytes) {
  std::string_view lenViewAll = req.headerValueOrEmpty(http::ContentLength);
  bool hasCL = !lenViewAll.empty();
  std::size_t headerEnd =
      static_cast<std::size_t>(req._flatHeaders.data() + req._flatHeaders.size() - state.buffer.data());
  if (!hasCL) {
    // No Content-Length and not chunked: treat as no body (common for GET/HEAD). Ready immediately.
    if (state.buffer.size() >= headerEnd) {
      req._body = std::string_view{};
      consumedBytes = headerEnd;
      return true;
    }
    return false;
  }
  std::size_t parsed = 0;
  auto [ptr, err] = std::from_chars(lenViewAll.data(), lenViewAll.data() + lenViewAll.size(), parsed);
  if (err != std::errc() || ptr != lenViewAll.data() + lenViewAll.size() || parsed > _config.maxBodyBytes) {
    parsed = _config.maxBodyBytes + 1;  // trigger error path (invalid or too large)
  }
  std::size_t contentLen = parsed;
  if (contentLen > _config.maxBodyBytes) {
    emitSimpleError(fd, state, 413, true);
    return false;
  }
  if (expectContinue && contentLen > 0) {
    queueData(fd, state, http::HTTP11_100_CONTINUE);
  }
  std::size_t totalNeeded = headerEnd + contentLen;
  if (state.buffer.size() < totalNeeded) {
    return false;  // need more bytes
  }
  req._body = {state.buffer.data() + headerEnd, contentLen};
  consumedBytes = totalNeeded;
  return true;
}

bool HttpServer::decodeChunkedBody(int fd, ConnectionState& state, HttpRequest& req, bool expectContinue,
                                   std::size_t& consumedBytes) {
  if (expectContinue) {
    queueData(fd, state, http::HTTP11_100_CONTINUE);
  }
  std::size_t pos = static_cast<std::size_t>(req._flatHeaders.data() + req._flatHeaders.size() - state.buffer.data());
  RawChars& decodedBody = state.bodyBuffer;
  decodedBody.clear();
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
      emitSimpleError(fd, state, 413, true);
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
      emitSimpleError(fd, state, 413, true);
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
  req._body = std::string_view(decodedBody);
  consumedBytes = pos;
  return true;
}

}  // namespace aeronet
