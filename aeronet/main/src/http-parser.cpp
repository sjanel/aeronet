#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <system_error>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "char-hexadecimal-converter.hpp"
#include "connection-state.hpp"
#include "raw-chars.hpp"

namespace aeronet {

bool HttpServer::decodeBodyIfReady(ConnectionMapIt cnxIt, HttpRequest& req, bool isChunked, bool expectContinue,
                                   std::size_t& consumedBytes) {
  consumedBytes = 0;
  if (isChunked) {
    return decodeChunkedBody(cnxIt, req, expectContinue, consumedBytes);
  }
  return decodeFixedLengthBody(cnxIt, req, expectContinue, consumedBytes);
}

bool HttpServer::decodeFixedLengthBody(ConnectionMapIt cnxIt, HttpRequest& req, bool expectContinue,
                                       std::size_t& consumedBytes) {
  std::string_view lenViewAll = req.headerValueOrEmpty(http::ContentLength);
  bool hasCL = !lenViewAll.empty();
  ConnectionState& state = cnxIt->second;
  std::size_t headerEnd =
      static_cast<std::size_t>(req._flatHeaders.data() + req._flatHeaders.size() - state.buffer.data());
  if (!hasCL) {
    // No Content-Length and not chunked: treat as no body (common for GET/HEAD). Ready immediately.
    // TODO: we should reject the query if body is non empty and ContentLength not specified
    if (state.buffer.size() >= headerEnd) {
      req._body = std::string_view{};
      consumedBytes = headerEnd;
      return true;
    }
    return false;
  }
  std::size_t declaredContentLen = 0;
  auto [ptr, err] = std::from_chars(lenViewAll.data(), lenViewAll.data() + lenViewAll.size(), declaredContentLen);
  if (err != std::errc() || ptr != lenViewAll.data() + lenViewAll.size()) {
    emitSimpleError(cnxIt, http::StatusCodeBadRequest, true, "Invalid Content-Length");
    return false;
  }
  if (declaredContentLen > _config.maxBodyBytes) {
    emitSimpleError(cnxIt, http::StatusCodePayloadTooLarge, true);
    return false;
  }
  if (expectContinue && declaredContentLen > 0) {
    queueData(cnxIt, HttpResponseData(http::HTTP11_100_CONTINUE));
  }
  std::size_t totalNeeded = headerEnd + declaredContentLen;
  if (state.buffer.size() < totalNeeded) {
    return false;  // need more bytes
  }
  req._body = {state.buffer.data() + headerEnd, declaredContentLen};
  consumedBytes = totalNeeded;
  return true;
}

bool HttpServer::decodeChunkedBody(ConnectionMapIt cnxIt, HttpRequest& req, bool expectContinue,
                                   std::size_t& consumedBytes) {
  ConnectionState& state = cnxIt->second;
  if (expectContinue) {
    queueData(cnxIt, HttpResponseData(http::HTTP11_100_CONTINUE));
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
    pos = static_cast<std::size_t>(lineEndIt - state.buffer.data()) + http::CRLF.size();
    if (chunkSize > _config.maxBodyBytes) {
      emitSimpleError(cnxIt, http::StatusCodePayloadTooLarge, true);
      return false;
    }
    if (state.buffer.size() < pos + chunkSize + http::CRLF.size()) {
      needMore = true;
      break;
    }
    if (chunkSize == 0) {
      if (state.buffer.size() < pos + http::CRLF.size()) {
        needMore = true;
        break;
      }
      if (std::memcmp(state.buffer.data() + pos, http::CRLF.data(), http::CRLF.size()) != 0) {
        needMore = true;
        break;
      }
      pos += http::CRLF.size();
      break;
    }
    decodedBody.append(state.buffer.data() + pos, std::min(chunkSize, state.buffer.size() - pos));
    if (decodedBody.size() > _config.maxBodyBytes) {
      emitSimpleError(cnxIt, http::StatusCodePayloadTooLarge, true);
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
