#pragma once

#include <cassert>
#include <charconv>
#include <concepts>
#include <cstring>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"

namespace aeronet {

constexpr char *WriteHeader(char *insertPtr, std::string_view key, std::string_view value) {
  std::memcpy(insertPtr, key.data(), key.size());
  std::memcpy(insertPtr + key.size(), http::HeaderSep.data(), http::HeaderSep.size());
  if (!value.empty()) {
    std::memcpy(insertPtr + key.size() + http::HeaderSep.size(), value.data(), value.size());
  }
  return insertPtr + key.size() + http::HeaderSep.size() + value.size();
}

constexpr char *WriteHeader(char *insertPtr, std::string_view key, std::integral auto value) {
  std::memcpy(insertPtr, key.data(), key.size());
  std::memcpy(insertPtr + key.size(), http::HeaderSep.data(), http::HeaderSep.size());
  const auto valueSz = nchars(value);
  insertPtr += key.size() + http::HeaderSep.size();
  [[maybe_unused]] const auto result = std::to_chars(insertPtr, insertPtr + valueSz, value);
  assert(result.ec == std::errc() && result.ptr == insertPtr + valueSz);
  return insertPtr + valueSz;
}

constexpr char *WriteCRLF(char *insertPtr) {
  std::memcpy(insertPtr, http::CRLF.data(), http::CRLF.size());
  return insertPtr + http::CRLF.size();
}

// Write an HTTP header field to the given buffer, including a last CRLF.
// Returns the pointer immediately after the last written byte.
// Header key must not be empty, but header value may be empty.
constexpr char *WriteHeaderCRLF(char *insertPtr, std::string_view key, std::string_view value) {
  return WriteCRLF(WriteHeader(insertPtr, key, value));
}

constexpr char *WriteHeaderCRLF(char *insertPtr, std::string_view key, std::integral auto value) {
  return WriteCRLF(WriteHeader(insertPtr, key, value));
}

// Same as above, but CRLF is first
constexpr char *WriteCRLFHeader(char *insertPtr, std::string_view key, std::string_view value) {
  return WriteHeader(WriteCRLF(insertPtr), key, value);
}

constexpr char *WriteCRLFHeader(char *insertPtr, std::string_view key, std::integral auto value) {
  return WriteHeader(WriteCRLF(insertPtr), key, value);
}

// Write a Date HTTP header field to the given buffer, including a last CRLF.
// Returns the pointer immediately after the last written byte.
// Given buffer requires a size of at least "Date".size() + HeaderSep.size() + kRFC7231DateStrLen + CRLF.size().
constexpr char *WriteCRLFDateHeader(char *insertPtr, SysTimePoint tp) {
  insertPtr = WriteCRLF(insertPtr);
  std::memcpy(insertPtr, http::Date.data(), http::Date.size());
  std::memcpy(insertPtr + http::Date.size(), http::HeaderSep.data(), http::HeaderSep.size());

  return TimeToStringRFC7231(tp, insertPtr + http::Date.size() + http::HeaderSep.size());
}

}  // namespace aeronet