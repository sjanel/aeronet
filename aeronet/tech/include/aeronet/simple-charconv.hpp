#pragma once

#include <concepts>
#include <cstdint>

#include "aeronet/http-status-code.hpp"

namespace aeronet {

constexpr auto write2(auto buf, std::integral auto value) noexcept {
  static constexpr char kDigitPairs[][2] = {
      {'0', '0'}, {'0', '1'}, {'0', '2'}, {'0', '3'}, {'0', '4'}, {'0', '5'}, {'0', '6'}, {'0', '7'}, {'0', '8'},
      {'0', '9'}, {'1', '0'}, {'1', '1'}, {'1', '2'}, {'1', '3'}, {'1', '4'}, {'1', '5'}, {'1', '6'}, {'1', '7'},
      {'1', '8'}, {'1', '9'}, {'2', '0'}, {'2', '1'}, {'2', '2'}, {'2', '3'}, {'2', '4'}, {'2', '5'}, {'2', '6'},
      {'2', '7'}, {'2', '8'}, {'2', '9'}, {'3', '0'}, {'3', '1'}, {'3', '2'}, {'3', '3'}, {'3', '4'}, {'3', '5'},
      {'3', '6'}, {'3', '7'}, {'3', '8'}, {'3', '9'}, {'4', '0'}, {'4', '1'}, {'4', '2'}, {'4', '3'}, {'4', '4'},
      {'4', '5'}, {'4', '6'}, {'4', '7'}, {'4', '8'}, {'4', '9'}, {'5', '0'}, {'5', '1'}, {'5', '2'}, {'5', '3'},
      {'5', '4'}, {'5', '5'}, {'5', '6'}, {'5', '7'}, {'5', '8'}, {'5', '9'}, {'6', '0'}, {'6', '1'}, {'6', '2'},
      {'6', '3'}, {'6', '4'}, {'6', '5'}, {'6', '6'}, {'6', '7'}, {'6', '8'}, {'6', '9'}, {'7', '0'}, {'7', '1'},
      {'7', '2'}, {'7', '3'}, {'7', '4'}, {'7', '5'}, {'7', '6'}, {'7', '7'}, {'7', '8'}, {'7', '9'}, {'8', '0'},
      {'8', '1'}, {'8', '2'}, {'8', '3'}, {'8', '4'}, {'8', '5'}, {'8', '6'}, {'8', '7'}, {'8', '8'}, {'8', '9'},
      {'9', '0'}, {'9', '1'}, {'9', '2'}, {'9', '3'}, {'9', '4'}, {'9', '5'}, {'9', '6'}, {'9', '7'}, {'9', '8'},
      {'9', '9'},
  };

  const char* pCouple = kDigitPairs[static_cast<uint8_t>(value)];
  buf[0] = pCouple[0];
  buf[1] = pCouple[1];
  return buf + 2;
}

constexpr auto write4(auto buf, std::integral auto value) noexcept {
  // Single div/mod by a compile-time constant (100), reusing write2's
  // table for both halves instead of extracting four digits separately.
  write2(buf, static_cast<unsigned>(value / 100));
  return write2(buf + 2, static_cast<unsigned>(value % 100));
}

// Write a valid status code (100-599) into buf, which must have room for at least 3 bytes. Returns the pointer
// immediately after the last written byte.
constexpr auto writeStatusCode(char* buf, http::StatusCode status) noexcept {
  buf[0] = static_cast<char>('1' + ((status - 100) / 100));
  return write2(buf + 1, static_cast<uint16_t>(status % 100));
}

constexpr auto write3(auto buf, std::integral auto value) noexcept {
  const auto lhs = value / 100;
  const auto rhs = value - (lhs * 100);

  *buf++ = static_cast<char>('0' + lhs);
  return write2(buf, rhs);
}

// Copy exactly 3 chars from a string_view known to have size >= 3.
constexpr auto copy3(auto des, auto src) {
  *des = src[0];
  *++des = src[1];
  *++des = src[2];
  return ++des;
}

constexpr uint8_t read2(const char* ptr) { return static_cast<uint8_t>(((ptr[0] - '0') * 10) + (ptr[1] - '0')); }

constexpr uint16_t read3(const char* ptr) {
  return static_cast<uint16_t>(((ptr[0] - '0') * 100) + ((ptr[1] - '0') * 10) + (ptr[2] - '0'));
}

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