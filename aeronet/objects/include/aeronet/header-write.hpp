#pragma once

#include <cassert>
#include <charconv>
#include <concepts>
#include <limits>
#include <string_view>

#ifndef NDEBUG
#include <system_error>
#endif

#include "aeronet/http-constants.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"

namespace aeronet {

constexpr char *WriteHeader(char *insertPtr, std::string_view key, std::string_view value) {
  insertPtr = Append(key, insertPtr);
  insertPtr = Append(http::HeaderSep, insertPtr);
  if (!value.empty()) {
    insertPtr = Append(value, insertPtr);
  }
  return insertPtr;
}

constexpr char *WriteHeader(char *insertPtr, std::string_view key, std::integral auto value) {
  insertPtr = Append(key, insertPtr);
  insertPtr = Append(http::HeaderSep, insertPtr);
  const auto [ptr, ec] =
      std::to_chars(insertPtr, insertPtr + std::numeric_limits<decltype(value)>::digits10 + 1, value);
  assert(ec == std::errc());
  return ptr;
}

// Write an HTTP header field to the given buffer, including a last CRLF.
// Returns the pointer immediately after the last written byte.
// Header key must not be empty, but header value may be empty.
constexpr char *WriteHeaderCRLF(char *insertPtr, std::string_view key, std::string_view value) {
  return Append(http::CRLF, WriteHeader(insertPtr, key, value));
}

constexpr char *WriteHeaderCRLF(char *insertPtr, std::string_view key, std::integral auto value) {
  return Append(http::CRLF, WriteHeader(insertPtr, key, value));
}

// Same as above, but CRLF is first
constexpr char *WriteCRLFHeader(char *insertPtr, std::string_view key, std::string_view value) {
  return WriteHeader(Append(http::CRLF, insertPtr), key, value);
}

constexpr char *WriteCRLFHeader(char *insertPtr, std::string_view key, std::integral auto value) {
  return WriteHeader(Append(http::CRLF, insertPtr), key, value);
}

// Write a Date HTTP header field to the given buffer, including a last CRLF.
// Returns the pointer immediately after the last written byte.
// Given buffer requires a size of at least "Date".size() + HeaderSep.size() + RFC7231DateStrLen + CRLF.size().
constexpr char *WriteCRLFDateHeader(char *insertPtr, SysTimePoint tp) {
  insertPtr = Append(http::CRLF, insertPtr);
  insertPtr = Append(http::Date, insertPtr);
  insertPtr = Append(http::HeaderSep, insertPtr);
  return TimeToStringRFC7231(tp, insertPtr);
}

}  // namespace aeronet