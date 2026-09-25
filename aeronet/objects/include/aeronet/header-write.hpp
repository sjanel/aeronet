#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <string_view>

#include "aeronet/decimal-writer.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/time-constants.hpp"

namespace aeronet {

constexpr char* WriteHeader(std::string_view key, std::string_view value, char* pData) {
  pData = Append(key, pData);
  pData = AppendFixed<http::HeaderSep>(pData);
  return Append(value, pData);
}

constexpr char* WriteHeader(std::string_view key, std::integral auto value, char* pData) {
  pData = Append(key, pData);
  pData = AppendFixed<http::HeaderSep>(pData);
  return WriteInt(pData, value, ndigits(value));
}

// Write an HTTP header field to the given buffer, including a last CRLF.
// Returns the pointer immediately after the last written byte.
// Header key must not be empty, but header value may be empty.
constexpr char* WriteHeaderCRLF(std::string_view key, std::string_view value, char* pData) {
  pData = WriteHeader(key, value, pData);
  return AppendFixed<http::CRLF>(pData);
}

// Same as above, but CRLF is first
constexpr char* WriteCRLFHeader(std::string_view key, std::string_view value, char* pData) {
  return WriteHeader(key, value, AppendFixed<http::CRLF>(pData));
}

constexpr char* WriteCRLFHeader(std::string_view key, std::integral auto value, char* pData) {
  return WriteHeader(key, value, AppendFixed<http::CRLF>(pData));
}

// Copy a previously formatted Date HTTP header, including its leading CRLF.
// Returns the pointer immediately after the copied bytes.
inline char* CopyCRLFDateHeader(const char* cachedHeader, char* pData) noexcept {
  assert(cachedHeader != nullptr);
  pData = AppendFixed<http::CRLFDateHeaderSep>(pData);

  std::memcpy(pData, cachedHeader, RFC7231DateStrLen);

  return pData + RFC7231DateStrLen;
}

constexpr char* WriteContentTypeContentLengthDoubleCRLF(std::string_view contentType, std::size_t bodySize,
                                                        char* pData) {
  pData = AppendFixed<http::ContentTypeHeaderSep>(pData);
  pData = Append(contentType, pData);
  pData = AppendFixed<http::CRLFContentLengthHeaderSep>(pData);
  pData = WriteUInt(pData, bodySize, ndigits(bodySize));

  return AppendFixed<http::DoubleCRLF>(pData);
}

}  // namespace aeronet
