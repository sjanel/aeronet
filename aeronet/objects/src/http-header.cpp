#include "aeronet/http-header.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string_view>

#include "aeronet/header-write.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/safe-cast.hpp"
#include "aeronet/string-trim.hpp"
#include "aeronet/tchars.hpp"

namespace aeronet::http {

Header::Header(std::string_view name, std::string_view value) : _nameLen(SafeCast<uint32_t>(name.size())) {
  value = TrimOws(value);
  _valueLen = SafeCast<uint32_t>(value.size());
  if (!IsValidHeaderName(name)) {
    throw std::invalid_argument("HTTP header name is invalid");
  }
  if (!IsValidHeaderValue(value)) {
    throw std::invalid_argument("HTTP header value is invalid");
  }
  _data = std::make_unique<char[]>(HeaderSep.size() + name.size() + value.size());

  WriteHeader(_data.get(), name, value);
}

Header::Header(const Header &rhs)
    : _data(std::make_unique<char[]>(HeaderSep.size() + rhs._nameLen + rhs._valueLen)),
      _nameLen(rhs._nameLen),
      _valueLen(rhs._valueLen) {
  std::memcpy(_data.get(), rhs._data.get(), HeaderSep.size() + rhs._nameLen + rhs._valueLen);
}

Header &Header::operator=(const Header &rhs) {
  if (this != &rhs) [[likely]] {
    const auto lhsTotalSize = HeaderSep.size() + _nameLen + _valueLen;
    const auto rhsTotalSize = HeaderSep.size() + rhs._nameLen + rhs._valueLen;
    if (lhsTotalSize < rhsTotalSize) {
      // Reallocate if current buffer is too small
      _data = std::make_unique<char[]>(rhsTotalSize);
    }
    _nameLen = rhs._nameLen;
    _valueLen = rhs._valueLen;
    std::memcpy(_data.get(), rhs._data.get(), rhsTotalSize);
  }
  return *this;
}

bool IsValidHeaderName(std::string_view name) noexcept {
  return !name.empty() && std::ranges::all_of(name, [](char ch) { return is_tchar(ch); });
}

bool IsValidHeaderValue(std::string_view value) noexcept {
  return std::ranges::all_of(value, [](unsigned char ch) {
    if (ch == '\r' || ch == '\n') {
      return false;
    }
    if (ch == '\t') {
      return true;
    }
    // Visible ASCII characters
    return ch >= 0x20 && ch <= 0x7E;
  });
}

}  // namespace aeronet::http