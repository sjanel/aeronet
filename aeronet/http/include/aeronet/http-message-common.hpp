#pragma once

#include <cstdint>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/static-string-view-helpers.hpp"
#include "aeronet/string-trim.hpp"

namespace aeronet {

// Cold paths of the HTTP messages: log the offending value, then throw (std::invalid_argument for the first,
// std::logic_error for the second). They are defined out of line to keep them out of the callers.
[[noreturn]] void ThrowInvalidContentTypeHeaderValue(std::string_view contentType);
[[noreturn]] void ThrowSetBodyAfterTrailers();

// Tells whether 'contentType' (already trimmed) can be the Content-Type value of a message with a body.
constexpr bool IsValidContentType(std::string_view contentType) {
  return contentType.size() >= http::ContentTypeMinLen && http::IsValidHeaderValue(contentType);
}

constexpr std::string_view CheckContentType(std::string_view& contentType) {
  contentType = TrimOws(contentType);
  if (!IsValidContentType(contentType)) [[unlikely]] {
    ThrowInvalidContentTypeHeaderValue(contentType);
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
