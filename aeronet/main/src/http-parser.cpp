#include <algorithm>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-server.hpp"
#include "char-hexadecimal-converter.hpp"
#include "http-constants.hpp"
#include "raw-chars.hpp"
#include "url-decode.hpp"

namespace aeronet {

bool HttpServer::decodeBodyIfReady(int fd, ConnectionState& state, const HttpRequest& req, bool isChunked,
                                   bool expectContinue, bool& closeConnection, std::size_t& consumedBytes) {
  consumedBytes = 0;
  if (!isChunked) {
    return decodeFixedLengthBody(fd, state, req, expectContinue, closeConnection, consumedBytes);
  }
  return decodeChunkedBody(fd, state, req, expectContinue, closeConnection, consumedBytes);
}

bool HttpServer::decodeFixedLengthBody(int fd, ConnectionState& state, const HttpRequest& req, bool expectContinue,
                                       bool& closeConnection, std::size_t& consumedBytes) {
  std::string_view lenViewAll = req.header(http::ContentLength);
  bool hasCL = !lenViewAll.empty();
  std::size_t contentLen = 0;
  std::size_t headerEnd =
      static_cast<std::size_t>(req._flatHeaders.data() + req._flatHeaders.size() - state.buffer.data());
  if (!hasCL) {
    // No Content-Length and not chunked: treat as no body (common for GET/HEAD). Ready immediately.
    if (state.buffer.size() >= headerEnd) {
      const_cast<HttpRequest&>(req)._body = std::string_view{};
      consumedBytes = headerEnd;
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
      emitSimpleError(fd, state, 413, closeConnection);
      return false;
    }
    if (expectContinue && contentLen > 0) {
      queueData(fd, state, http::HTTP11_100_CONTINUE);
    }
  }
  std::size_t totalNeeded = headerEnd + contentLen;
  if (state.buffer.size() < totalNeeded) {
    return false;  // need more bytes
  }
  const char* bodyStart = state.buffer.data() + headerEnd;
  const_cast<HttpRequest&>(req)._body = {bodyStart, contentLen};
  consumedBytes = totalNeeded;
  return true;
}

bool HttpServer::decodeChunkedBody(int fd, ConnectionState& state, const HttpRequest& req, bool expectContinue,
                                   bool& closeConnection, std::size_t& consumedBytes) {
  if (expectContinue) {
    queueData(fd, state, http::HTTP11_100_CONTINUE);
  }
  std::size_t pos = static_cast<std::size_t>(req._flatHeaders.data() + req._flatHeaders.size() - state.buffer.data());
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
      emitSimpleError(fd, state, 413, closeConnection);
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
      emitSimpleError(fd, state, 413, closeConnection);
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
  const_cast<HttpRequest&>(req)._body = std::string_view(state.bodyStorage);
  consumedBytes = pos;
  return true;
}

}  // namespace aeronet
