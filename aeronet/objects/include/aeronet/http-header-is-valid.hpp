#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/tchars.hpp"

namespace aeronet::http {

namespace detail {

// Classes of the bytes of a header field (RFC 9110 §5.5, §5.6.2), as bits of a single table.
// field-value = *( field-vchar / SP / HTAB ), field-vchar = VCHAR / obs-text (bytes 0x80-0xFF): CR, LF, NUL and the
// other control characters are never allowed.
inline constexpr std::uint8_t kReceivedValueChar = 1U << 0U;  // HTAB, SP, VCHAR and obs-text
inline constexpr std::uint8_t kValueChar = 1U << 1U;          // HTAB, SP and VCHAR: what aeronet sends
inline constexpr std::uint8_t kNameChar = 1U << 2U;           // tchar (a field-name is a token)

// Returns true if every byte of 'str' is of class 'classBit'.
// Branchless (the classes of all the bytes are AND-ed) as header fields are almost always valid.
constexpr bool AllCharsOfClass(std::string_view str, std::uint8_t classBit) noexcept {
  struct ClassTable {
    std::uint8_t data[256];
  };
  static constexpr auto kCharClasses = [] {
    ClassTable table;
    for (std::size_t ch = 0; ch < sizeof(table.data); ++ch) {
      const bool isValueChar = ch == '\t' || (ch >= 0x20 && ch <= 0x7E);
      table.data[ch] = static_cast<std::uint8_t>((isValueChar || ch >= 0x80 ? kReceivedValueChar : 0U) |
                                                 (isValueChar ? kValueChar : 0U) |
                                                 (is_tchar(static_cast<unsigned char>(ch)) ? kNameChar : 0U));
    }
    return table;
  }();

  std::uint8_t common = classBit;
  for (auto ch : str) {
    common &= kCharClasses.data[static_cast<unsigned char>(ch)];
  }
  return common != 0;
}

}  // namespace detail

// Returns true if the header name is valid according to RFC 7230 §3.2.6 (tchar characters only, no whitespace, not
// empty).
constexpr bool IsValidHeaderName(std::string_view name) noexcept {
  return !name.empty() && detail::AllCharsOfClass(name, detail::kNameChar);
}

// Returns true if the header value only contains visible US-ASCII characters, SP and HTAB: what aeronet sends
// (RFC 9110 §5.5 - "Newly defined fields SHOULD limit their values to visible US-ASCII octets (VCHAR), SP, and HTAB").
constexpr bool IsValidHeaderValue(std::string_view value) noexcept {
  return detail::AllCharsOfClass(value, detail::kValueChar);
}

// Returns true if the received header value is valid according to RFC 9110 §5.5: like IsValidHeaderValue, but also
// accepting obs-text (bytes 0x80-0xFF, such as UTF-8 text), kept as opaque data.
constexpr bool IsValidReceivedHeaderValue(std::string_view value) noexcept {
  return detail::AllCharsOfClass(value, detail::kReceivedValueChar);
}

}  // namespace aeronet::http
