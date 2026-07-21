#pragma once

#include <charconv>
#include <concepts>
#include <cstring>
#include <limits>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/memory-utils-sv.hpp"
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

  return std::to_chars(insertPtr, insertPtr + std::numeric_limits<decltype(value)>::digits10 + 1, value).ptr;
}

// Write an HTTP header field to the given buffer, including a last CRLF.
// Returns the pointer immediately after the last written byte.
// Header key must not be empty, but header value may be empty.
constexpr char* WriteHeaderCRLF(std::string_view key, std::string_view value, char* insertPtr) {
  return Append(http::CRLF, WriteHeader(key, value, insertPtr));
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
  std::memcpy(insertPtr, http::CRLFDateHeaderSep.data(), http::CRLFDateHeaderSep.size());
  return TimeToStringRFC7231(tp, insertPtr + http::CRLFDateHeaderSep.size());
}

inline char* WriteContentTypeContentLengthDoubleCRLF(std::string_view contentType, std::size_t bodySize, char* pData) {
  std::memcpy(pData, http::ContentTypeHeaderSep.data(), http::ContentTypeHeaderSep.size());
  pData += http::ContentTypeHeaderSep.size();

  pData = Append(contentType, pData);

  std::memcpy(pData, http::CRLFContentLengthHeaderSep.data(), http::CRLFContentLengthHeaderSep.size());
  pData += http::CRLFContentLengthHeaderSep.size();

  pData = std::to_chars(pData, pData + std::numeric_limits<std::size_t>::digits10 + 1, bodySize).ptr;

  std::memcpy(pData, http::DoubleCRLF.data(), http::DoubleCRLF.size());
  pData += http::DoubleCRLF.size();
  return pData;
}

}  // namespace aeronet