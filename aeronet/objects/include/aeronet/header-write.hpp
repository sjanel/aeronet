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

constexpr char* WriteHeader(std::string_view key, std::string_view value, char* insertPtr) {
  insertPtr = Append(key, insertPtr);
  insertPtr = AppendFixed<http::HeaderSep>(insertPtr);
  return Append(value, insertPtr);
}

constexpr char* WriteHeader(std::string_view key, std::integral auto value, char* insertPtr) {
  insertPtr = Append(key, insertPtr);
  insertPtr = AppendFixed<http::HeaderSep>(insertPtr);
  return WriteInt(insertPtr, value, ndigits(value));
}

// Write an HTTP header field to the given buffer, including a last CRLF.
// Returns the pointer immediately after the last written byte.
// Header key must not be empty, but header value may be empty.
constexpr char* WriteHeaderCRLF(std::string_view key, std::string_view value, char* insertPtr) {
  insertPtr = WriteHeader(key, value, insertPtr);
  return AppendFixed<http::CRLF>(insertPtr);
}

// Same as above, but CRLF is first
constexpr char* WriteCRLFHeader(std::string_view key, std::string_view value, char* insertPtr) {
  return WriteHeader(key, value, AppendFixed<http::CRLF>(insertPtr));
}

constexpr char* WriteCRLFHeader(std::string_view key, std::integral auto value, char* insertPtr) {
  return WriteHeader(key, value, AppendFixed<http::CRLF>(insertPtr));
}

// Copy a previously formatted Date HTTP header, including its leading CRLF.
// Returns the pointer immediately after the copied bytes.
inline char* CopyCRLFDateHeader(const char* cachedHeader, char* insertPtr) noexcept {
  assert(cachedHeader != nullptr);
  insertPtr = AppendFixed<http::CRLFDateHeaderSep>(insertPtr);

  std::memcpy(insertPtr, cachedHeader, RFC7231DateStrLen);

  return insertPtr + RFC7231DateStrLen;
}

inline char* WriteContentTypeContentLengthDoubleCRLF(std::string_view contentType, std::size_t bodySize, char* pData) {
  pData = AppendFixed<http::ContentTypeHeaderSep>(pData);

  pData = Append(contentType, pData);

  pData = AppendFixed<http::CRLFContentLengthHeaderSep>(pData);

  pData = WriteUInt(pData, bodySize, ndigits(bodySize));

  return AppendFixed<http::DoubleCRLF>(pData);
}

}  // namespace aeronet
