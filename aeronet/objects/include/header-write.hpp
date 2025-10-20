#pragma once

#include <cstring>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "timedef.hpp"
#include "timestring.hpp"

namespace aeronet {

inline char *WriteHeader(char *insertPtr, std::string_view key, std::string_view value) {
  std::memcpy(insertPtr, key.data(), key.size());
  std::memcpy(insertPtr + key.size(), http::HeaderSep.data(), http::HeaderSep.size());
  if (!value.empty()) {
    std::memcpy(insertPtr + key.size() + http::HeaderSep.size(), value.data(), value.size());
  }
  return insertPtr + key.size() + http::HeaderSep.size() + value.size();
}

inline char *WriteCRLF(char *insertPtr) {
  std::memcpy(insertPtr, http::CRLF.data(), http::CRLF.size());
  return insertPtr + http::CRLF.size();
}

// Write an HTTP header field to the given buffer, including a last CRLF.
// Returns the pointer immediately after the last written byte.
// Header key must not be empty, but header value may be empty.
inline char *WriteHeaderCRLF(char *insertPtr, std::string_view key, std::string_view value) {
  return WriteCRLF(WriteHeader(insertPtr, key, value));
}

// Same as above, but CRLF is first
inline char *WriteCRLFHeader(char *insertPtr, std::string_view key, std::string_view value) {
  return WriteHeader(WriteCRLF(insertPtr), key, value);
}

// Write a Date HTTP header field to the given buffer, including a last CRLF.
// Returns the pointer immediately after the last written byte.
// Given buffer requires a size of at least "Date".size() + HeaderSep.size() + kRFC7231DateStrLen + CRLF.size().
inline char *WriteCRLFDateHeader(char *insertPtr, SysTimePoint tp) {
  insertPtr = WriteCRLF(insertPtr);
  std::memcpy(insertPtr, http::Date.data(), http::Date.size());
  std::memcpy(insertPtr + http::Date.size(), http::HeaderSep.data(), http::HeaderSep.size());

  return TimeToStringRFC7231(tp, insertPtr + http::Date.size() + http::HeaderSep.size());
}

}  // namespace aeronet