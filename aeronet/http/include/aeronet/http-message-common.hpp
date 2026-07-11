#pragma once

#include <algorithm>
#include <stdexcept>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/static-string-view-helpers.hpp"
#include "aeronet/string-trim.hpp"

namespace aeronet {

constexpr void CheckConcatenatedHeaders(std::string_view concatenatedHeaders) {
  const char* first = concatenatedHeaders.data();
  const char* last = first + concatenatedHeaders.size();

  while (first < last) {
    const char* headerNameEnd = std::search(first, last, http::HeaderSep.begin(), http::HeaderSep.end());
    if (headerNameEnd == last) {
      throw std::invalid_argument("header missing http::HeaderSep separator in concatenated headers");
    }

    std::string_view headerName(first, headerNameEnd);
    if (!http::IsValidHeaderName(headerName)) {
      throw std::invalid_argument("Invalid header name in concatenated headers");
    }
    first += headerName.size() + http::HeaderSep.size();

    const char* endLine = SearchCRLF(first, last);
    if (endLine == last) {
      throw std::invalid_argument("header missing CRLF terminator in concatenated headers");
    }

    std::string_view headerValue(first, endLine);
    if (!http::IsValidHeaderValue(headerValue)) {
      throw std::invalid_argument("Invalid header value in concatenated headers");
    }

    first = endLine + http::CRLF.size();
  }
}

constexpr std::string_view CheckContentType(bool isBodyEmpty, std::string_view& contentType) {
  contentType = TrimOws(contentType);
  if (!isBodyEmpty && (contentType.size() < http::ContentTypeMinLen || !http::IsValidHeaderValue(contentType)))
      [[unlikely]] {
    throw std::invalid_argument("HTTP content-type header value is invalid");
  }
  return contentType;
}

inline constexpr std::string_view kTrailerValueSep = ", ";

inline constexpr std::string_view kTransferEncodingChunkedCRLF =
    JoinStringView_v<http::TransferEncoding, http::HeaderSep, http::chunked, http::CRLF>;

// Returns the size difference between the new Transfer-Encoding: chunked header and Content-Length header.
// For very large payloads, this can be negative, as Content-Length can be larger than
// Transfer-Encoding: chunked.
constexpr int64_t TransferEncodingHeaderSizeDiff(std::uint8_t nCharsBodyLen) {
  const auto oldContentLengthHeaderSize = http::HeaderSize(http::ContentLength.size(), nCharsBodyLen);

  return static_cast<int64_t>(kTransferEncodingChunkedCRLF.size()) - static_cast<int64_t>(oldContentLengthHeaderSize);
}

}  // namespace aeronet