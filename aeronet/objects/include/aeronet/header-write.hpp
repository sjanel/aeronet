#pragma once

#include <concepts>
#include <cstddef>
#include <string_view>

#include "aeronet/decimal-writer.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"

namespace aeronet {

constexpr char* WriteHeader(std::string_view key, std::string_view value, char* insertPtr) {
  insertPtr = Append(key, insertPtr);
  insertPtr = Append(http::HeaderSep, insertPtr);
  return Append(value, insertPtr);
}

constexpr char* WriteHeader(std::string_view key, std::integral auto value, char* insertPtr) {
  insertPtr = Append(key, insertPtr);
  insertPtr = Append(http::HeaderSep, insertPtr);
  return WriteInt(insertPtr, value, ndigits(value));
}

// Write an HTTP header field to the given buffer, including a last CRLF.
// Returns the pointer immediately after the last written byte.
// Header key must not be empty, but header value may be empty.
constexpr char* WriteHeaderCRLF(std::string_view key, std::string_view value, char* insertPtr) {
  char* pData = WriteHeader(key, value, insertPtr);
  return Append(http::CRLF, pData);
}

// Same as above, but CRLF is first
constexpr char* WriteCRLFHeader(std::string_view key, std::string_view value, char* insertPtr) {
  return WriteHeader(key, value, Append(http::CRLF, insertPtr));
}

constexpr char* WriteCRLFHeader(std::string_view key, std::integral auto value, char* insertPtr) {
  return WriteHeader(key, value, Append(http::CRLF, insertPtr));
}

// Write a Date HTTP header field to the given buffer, including a last CRLF.
// Returns the pointer immediately after the last written byte.
// Given buffer requires a size of at least "Date".size() + HeaderSep.size() + RFC7231DateStrLen + CRLF.size().
inline char* WriteCRLFDateHeader(SysTimePoint tp, char* insertPtr) {
  return TimeToStringRFC7231(tp, Append(http::CRLFDateHeaderSep, insertPtr));
}

inline char* WriteContentTypeContentLengthDoubleCRLF(std::string_view contentType, std::size_t bodySize, char* pData) {
  pData = Append(http::ContentTypeHeaderSep, pData);

  pData = Append(contentType, pData);

  pData = Append(http::CRLFContentLengthHeaderSep, pData);

  pData = WriteUInt(pData, bodySize, ndigits(bodySize));

  return Append(http::DoubleCRLF, pData);
}

}  // namespace aeronet