#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/http-header-is-valid.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/string-trim.hpp"

namespace aeronet::http {

// The parts of an HTTP/1.x status line, as parsed by ParseStatusLine().
struct StatusLine {
  // Whether the line is a status line. When it is not, all fields are empty.
  [[nodiscard]] constexpr bool valid() const noexcept { return code != 0; }

  std::string_view reason;  // without surrounding whitespace, empty when absent
  StatusCode code{0};       // from 100 to 599
  Version version;          // of major version 1
};

// Size of the shortest status line, "HTTP/1.1 200": without reason phrase, nor the SP before it.
inline constexpr std::size_t kStatusLineMinLen = Version::kStrLen + 1U + StatusCodeLen;

// Parses `line`, a status line without its CRLF (RFC 9112 §4):
//   status-line = HTTP-version SP status-code SP [ reason-phrase ]
// HTTP-version must be "HTTP/1." DIGIT (case-sensitive), status-code 3 digits from 100 to 599, and reason-phrase
// 1*( HTAB / SP / VCHAR / obs-text ). The SP after the status code may be omitted with the reason phrase, as some
// servers do. Any other line gives an invalid StatusLine.
// Every prefix of a status line holding its status code is itself a status line: the part of a line received so far
// can be checked before its end.
constexpr StatusLine ParseStatusLine(std::string_view line) noexcept {
  if (line.size() < kStatusLineMinLen || line[Version::kStrLen] != ' ') {
    return {};
  }
  // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
  const Version version = Version::OfMajor<1>(line.data(), Version::kStrLen);
  // Value of the digit at pos of the status code, above 9 for any other character.
  const auto digit = [pCode = line.data() + Version::kStrLen + 1U](std::size_t pos) {
    return static_cast<unsigned char>(pCode[pos] - '0');
  };
  const unsigned hundreds = digit(0);
  const unsigned tens = digit(1);
  const unsigned units = digit(2);
  if (!version.isValid() || hundreds < 1U || hundreds > 5U || tens > 9U || units > 9U) {
    return {};
  }
  std::string_view reason;
  if (line.size() != kStatusLineMinLen) {
    if (line[kStatusLineMinLen] != ' ') {
      return {};
    }
    reason = TrimOws(line.substr(kStatusLineMinLen + 1U));
    if (!IsValidReceivedHeaderValue(reason)) {
      return {};
    }
  }
  const auto code = static_cast<StatusCode>((hundreds * 100U) + (tens * 10U) + units);
  return {.reason = reason, .code = code, .version = version};
}

}  // namespace aeronet::http
