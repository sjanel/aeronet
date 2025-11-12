#pragma once

#include <concepts>

namespace aeronet {

constexpr auto write2(auto buf, std::integral auto value) {
  *buf = static_cast<char>('0' + (value / 10));
  *++buf = static_cast<char>('0' + (value % 10));
  return ++buf;
}

constexpr auto write3(auto buf, std::integral auto value) {
  *buf = static_cast<char>('0' + (value / 100));
  *++buf = static_cast<char>('0' + ((value / 10) % 10));
  *++buf = static_cast<char>('0' + (value % 10));
  return ++buf;
}

constexpr auto write4(auto buf, std::integral auto value) {
  *buf = static_cast<char>('0' + (value / 1000));
  *++buf = static_cast<char>('0' + ((value / 100) % 10));
  *++buf = static_cast<char>('0' + ((value / 10) % 10));
  *++buf = static_cast<char>('0' + (value % 10));
  return ++buf;
}

// Copy exactly 3 chars from a string_view known to have size >= 3.
constexpr auto copy3(auto des, auto src) {
  *des = src[0];
  *++des = src[1];
  *++des = src[2];
  return ++des;
}

constexpr auto read2(const char* ptr) { return ((ptr[0] - '0') * 10) + (ptr[1] - '0'); }

constexpr auto read3(const char* ptr) { return ((ptr[0] - '0') * 100) + ((ptr[1] - '0') * 10) + (ptr[2] - '0'); }

constexpr auto read4(const char* ptr) {
  return ((ptr[0] - '0') * 1000) + ((ptr[1] - '0') * 100) + ((ptr[2] - '0') * 10) + (ptr[3] - '0');
}

constexpr auto read6(const char* ptr) {
  return ((ptr[0] - '0') * 100000) + ((ptr[1] - '0') * 10000) + ((ptr[2] - '0') * 1000) + ((ptr[3] - '0') * 100) +
         ((ptr[4] - '0') * 10) + (ptr[5] - '0');
}

constexpr auto read9(const char* ptr) {
  return ((ptr[0] - '0') * 100000000) + ((ptr[1] - '0') * 10000000) + ((ptr[2] - '0') * 1000000) +
         ((ptr[3] - '0') * 100000) + ((ptr[4] - '0') * 10000) + ((ptr[5] - '0') * 1000) + ((ptr[6] - '0') * 100) +
         ((ptr[7] - '0') * 10) + (ptr[8] - '0');
}

}  // namespace aeronet