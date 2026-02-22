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

constexpr char* WriteHeader(std::string_view key, std::string_view value, char* insertPtr) {
  insertPtr = Append(key, insertPtr);
  insertPtr = Append(http::HeaderSep, insertPtr);
  if (!value.empty()) {
    insertPtr = Append(value, insertPtr);
  }
  return insertPtr;
}

constexpr char* WriteHeader(std::string_view key, std::integral auto value, char* insertPtr) {
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
constexpr char* WriteHeaderCRLF(std::string_view key, std::string_view value, char* insertPtr) {
  return Append(http::CRLF, WriteHeader(key, value, insertPtr));
}

constexpr char* WriteHeaderCRLF(std::string_view key, std::integral auto value, char* insertPtr) {
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
constexpr char* WriteCRLFDateHeader(SysTimePoint tp, char* insertPtr) {
  insertPtr = Append(http::CRLF, insertPtr);
  insertPtr = Append(http::Date, insertPtr);
  insertPtr = Append(http::HeaderSep, insertPtr);
  return TimeToStringRFC7231(tp, insertPtr);
}

}  // namespace aeronet