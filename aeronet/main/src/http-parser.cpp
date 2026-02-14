#include <algorithm>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <string_view>
#include <system_error>

#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/header-line-parse.hpp"
#include "aeronet/header-merge.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-data.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/reserved-headers.hpp"
#include "aeronet/single-http-server.hpp"

namespace aeronet {

SingleHttpServer::BodyDecodeStatus SingleHttpServer::decodeBodyIfReady(ConnectionMapIt cnxIt, bool isChunked,
                                                                       bool expectContinue,
                                                                       std::size_t& consumedBytes) {
  consumedBytes = 0;
  if (isChunked) {
    return decodeChunkedBody(cnxIt, expectContinue, consumedBytes);
  }
  // For fixed-length, non-chunked HTTP/1.1 requests there are no trailers per RFC 7230 §4.1.2
  return decodeFixedLengthBody(cnxIt, expectContinue, consumedBytes);
}

SingleHttpServer::BodyDecodeStatus SingleHttpServer::decodeFixedLengthBody(ConnectionMapIt cnxIt, bool expectContinue,
                                                                           std::size_t& consumedBytes) {
  ConnectionState& state = *cnxIt->second;
  HttpRequest& request = state.request;
  auto optContentLength = request.headerValue(http::ContentLength);
  const std::size_t headerEnd = request.headSpanSize();
  if (!optContentLength) {
    // No Content-Length and not chunked: treat as no body (common for GET/HEAD). Ready immediately.
    request._body = std::string_view{};
    consumedBytes = headerEnd;
    return BodyDecodeStatus::Ready;
  }
  std::string_view lenViewAll = *optContentLength;
  // Note: in HTTP/1.1, trailers are not allowed for non-chunked bodies.
  std::size_t declaredContentLen = 0;
  auto [ptr, err] = std::from_chars(lenViewAll.data(), lenViewAll.data() + lenViewAll.size(), declaredContentLen);
  if (err != std::errc() || ptr != lenViewAll.data() + lenViewAll.size()) {
    emitSimpleError(cnxIt, http::StatusCodeBadRequest, true, "Invalid Content-Length");
    return BodyDecodeStatus::Error;
  }
  if (_config.maxBodyBytes < declaredContentLen) {
    emitSimpleError(cnxIt, http::StatusCodePayloadTooLarge, true, {});
    return BodyDecodeStatus::Error;
  }
  if (expectContinue && declaredContentLen > 0) {
    queueData(cnxIt, HttpResponseData(RawChars{}, HttpPayload(http::HTTP11_100_CONTINUE)));
  }
  std::size_t totalNeeded = headerEnd + declaredContentLen;
  if (state.inBuffer.size() < totalNeeded) {
    return BodyDecodeStatus::NeedMore;  // need more bytes
  }
  request._body = {state.inBuffer.data() + headerEnd, declaredContentLen};
  consumedBytes = totalNeeded;
  return BodyDecodeStatus::Ready;
}

SingleHttpServer::BodyDecodeStatus SingleHttpServer::decodeChunkedBody(ConnectionMapIt cnxIt, bool expectContinue,
                                                                       std::size_t& consumedBytes) {
  ConnectionState& state = *cnxIt->second;
  HttpRequest& request = state.request;
  if (expectContinue) {
    queueData(cnxIt, HttpResponseData(RawChars{}, HttpPayload(http::HTTP11_100_CONTINUE)));
  }
  std::size_t pos = request.headSpanSize();
  RawChars& bodyAndTrailers = state.bodyAndTrailersBuffer;
  bodyAndTrailers.clear();
  state.trailerStartPos = 0;

  // Check if we should use direct decompression (avoids copying compressed chunks to bodyAndTrailers)
  const http::StatusCode decompressCode = internal::HttpCodec::WillDecompress(_config.decompression, request.headers());
  if (decompressCode == http::StatusCodeBadRequest) {
    emitSimpleError(cnxIt, http::StatusCodeBadRequest, true, "Malformed Content-Encoding");
    return BodyDecodeStatus::Error;
  }

  // For direct decompression, we collect chunk positions instead of copying data
  _tmp.sv.clear();
  std::size_t totalCompressedSize = 0;

  while (true) {
    auto first = state.inBuffer.begin() + pos;
    auto last = state.inBuffer.end();
    auto lineEnd = std::search(first, last, http::CRLF.begin(), http::CRLF.end());
    if (lineEnd == last) {
      return BodyDecodeStatus::NeedMore;
    }
    const auto sizeLineEnd = std::find(first, lineEnd, ';');  // ignore chunk extensions per RFC 7230 section 4.1.1
    std::size_t chunkSize = 0;
    for (auto it = first; it != sizeLineEnd; ++it) {
      const int8_t digit = from_hex_digit(*it);
      if (digit < 0) {
        emitSimpleError(cnxIt, http::StatusCodeBadRequest, true, "Invalid chunk size");
        return BodyDecodeStatus::Error;
      }
      chunkSize = (chunkSize << 4) | static_cast<std::size_t>(digit);
      if (_config.maxBodyBytes < chunkSize) {
        emitSimpleError(cnxIt, http::StatusCodePayloadTooLarge, true, {});
        return BodyDecodeStatus::Error;
      }
    }
    pos = static_cast<std::size_t>(lineEnd - state.inBuffer.data()) + http::CRLF.size();
    if (state.inBuffer.size() < pos + chunkSize + http::CRLF.size()) {
      return BodyDecodeStatus::NeedMore;
    }

    if (chunkSize == 0) {
      // Zero-chunk detected. Now parse optional trailer headers (RFC 7230 §4.1.2).
      // Trailers are terminated by a blank line (CRLF).

      // Store body size before appending trailers to the same buffer
      const std::size_t bodySize = decompressCode == http::StatusCodeOK ? 0 : bodyAndTrailers.size();

      // First, check if we have at least the immediate terminating CRLF
      if (state.inBuffer.size() < pos + http::CRLF.size()) {
        return BodyDecodeStatus::NeedMore;
      }

      // Check if trailers are present (not immediate CRLF)
      auto* trailerStart = state.inBuffer.data() + pos;
      if (std::memcmp(trailerStart, http::CRLF.data(), http::CRLF.size()) == 0) {
        // No trailers, just the terminating CRLF
        pos += http::CRLF.size();
        break;
      }

      // Parse trailer headers - copy raw trailer data to bodyAndTrailers buffer
      state.trailerStartPos = bodySize;  // Mark where trailers begin in the buffer
      std::size_t trailerEndPos = pos;   // Track end position for later

      // First pass: validate trailers and find the end position
      std::size_t tempPos = pos;
      while (true) {
        auto lineEndIt = std::search(state.inBuffer.begin() + tempPos, last, http::CRLF.begin(), http::CRLF.end());
        if (lineEndIt == last) {
          return BodyDecodeStatus::NeedMore;
        }

        // Check total trailer size limit
        const std::size_t trailerSize = static_cast<std::size_t>((state.inBuffer.data() + tempPos) - trailerStart);
        if (trailerSize > _config.maxHeaderBytes) {
          emitSimpleError(cnxIt, http::StatusCodeRequestHeaderFieldsTooLarge, true);
          return BodyDecodeStatus::Error;
        }

        auto* lineStart = state.inBuffer.data() + tempPos;
        auto* lineLast = lineEndIt;

        // Detect blank line (end of trailers)
        if (lineStart == lineLast || (std::distance(lineStart, lineLast) == 1 && *lineStart == '\r')) {
          trailerEndPos = static_cast<std::size_t>(lineEndIt - state.inBuffer.data()) + http::CRLF.size();
          break;
        }

        // Parse trailer field: name:value
        auto* colonPtr = std::find(lineStart, lineLast, ':');
        if (colonPtr == lineLast) {
          emitSimpleError(cnxIt, http::StatusCodeBadRequest, true, "Malformed trailer header");
          return BodyDecodeStatus::Error;
        }

        // Check forbidden headers
        if (http::IsForbiddenTrailerHeader(std::string_view(lineStart, colonPtr))) {
          emitSimpleError(cnxIt, http::StatusCodeBadRequest, true, "Forbidden trailer header");
          return BodyDecodeStatus::Error;
        }

        tempPos = static_cast<std::size_t>(lineEndIt - state.inBuffer.data()) + http::CRLF.size();
      }

      // Copy all trailer data at once to avoid reallocation during parsing
      std::size_t trailerDataSize = trailerEndPos - pos - http::CRLF.size();  // Exclude final CRLF
      bodyAndTrailers.append(trailerStart, trailerDataSize);

      // Second pass: parse trailers from copied data in bodyAndTrailers
      char* trailerDataBeg = bodyAndTrailers.data() + state.trailerStartPos;
      char* trailerDataEnd = bodyAndTrailers.data() + bodyAndTrailers.size();

      if (!parseHeadersUnchecked(request._trailers, bodyAndTrailers.data(), trailerDataBeg, trailerDataEnd)) {
        emitSimpleError(cnxIt, http::StatusCodeBadRequest, true, "Invalid trailer headers");
        return BodyDecodeStatus::Error;
      }

      pos = trailerEndPos;

      break;
    }

    if (decompressCode == http::StatusCodeOK) {
      // Just record chunk position for later direct decompression
      _tmp.sv.emplace_back(state.inBuffer.data() + pos, chunkSize);
      totalCompressedSize += chunkSize;

      if (totalCompressedSize > _config.maxBodyBytes ||
          (_config.decompression.maxCompressedBytes != 0 &&
           totalCompressedSize > _config.decompression.maxCompressedBytes)) {
        emitSimpleError(cnxIt, http::StatusCodePayloadTooLarge, true);
        return BodyDecodeStatus::Error;
      }
    } else {
      // Append chunk data to body buffer (original path)
      const auto appendSz = std::min(chunkSize, state.inBuffer.size() - pos);

      if (bodyAndTrailers.size() + appendSz > _config.maxBodyBytes) {
        emitSimpleError(cnxIt, http::StatusCodePayloadTooLarge, true);
        return BodyDecodeStatus::Error;
      }

      bodyAndTrailers.append(state.inBuffer.data() + pos, appendSz);
    }
    pos += chunkSize;
    if (std::memcmp(state.inBuffer.data() + pos, http::CRLF.data(), http::CRLF.size()) != 0) {
      return BodyDecodeStatus::NeedMore;
    }
    pos += http::CRLF.size();
  }

  if (decompressCode == http::StatusCodeOK && !_tmp.sv.empty()) {
    // Perform direct decompression from inBuffer chunks to bodyAndTrailersBuffer
    // Save trailers if present (they were appended to bodyAndTrailers with trailerStartPos = 0)
    // In direct decompression mode, bodyAndTrailers only contains trailers (no body chunks were copied)
    const bool hasTrailers = !bodyAndTrailers.empty();

    _tmp.trailers.assign(bodyAndTrailers);

    const auto res = internal::HttpCodec::DecompressChunkedBody(
        _decompressionState, _config.decompression, request, _tmp.sv, totalCompressedSize, bodyAndTrailers, _tmp.buf);
    if (res.message != nullptr) {
      emitSimpleError(cnxIt, res.status, true, res.message);
      return BodyDecodeStatus::Error;
    }

    // Restore trailers if present
    if (hasTrailers) {
      state.trailerStartPos = bodyAndTrailers.size();
      // Capacity have been reserved in DecompressChunkedBody, so unchecked append is safe here.
      // In addition, a realloc here would mean that we should re-compute the request._body that is set in
      // DecompressChunkedBody.
      assert(bodyAndTrailers.capacity() >= bodyAndTrailers.size() + _tmp.trailers.size());
      bodyAndTrailers.unchecked_append(_tmp.trailers);
    }

    // Body is set by DecompressChunkedBodyDirect, trailers are appended after
  } else {
    // Body is everything before trailerStartPos (or entire buffer if no trailers)
    std::size_t bodyLen = (state.trailerStartPos > 0) ? state.trailerStartPos : bodyAndTrailers.size();
    request._body = std::string_view(bodyAndTrailers.data(), bodyLen);
  }

  consumedBytes = pos;
  return BodyDecodeStatus::Ready;
}

bool SingleHttpServer::parseHeadersUnchecked(HeadersViewMap& headersMap, char* bufferBeg, char* first, char* last) {
  headersMap.clear();
  while (first < last) {
    // Find line end
    char* lineEnd = std::search(first, last, http::CRLF.begin(), http::CRLF.end());
    if (lineEnd == last) {
      break;  // No more lines
    }

    // No check is made on header line format here
    const auto [headerName, headerValue] = http::ParseHeaderLine(first, lineEnd);

    // Store trailer using the in-place merge helper so semantics/pointer updates match request parsing.
    if (!http::AddOrMergeHeaderInPlace(headersMap, headerName, headerValue, _tmp.buf, bufferBeg, first,
                                       _config.mergeUnknownRequestHeaders)) {
      return false;
    }

    first = lineEnd + http::CRLF.size();
  }
  return true;
}

}  // namespace aeronet
