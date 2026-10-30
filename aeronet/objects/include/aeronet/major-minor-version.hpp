#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/static-string-view-helpers.hpp"

namespace aeronet {

// Single digit Major.Minor version representation, e.g. HTTP/1.1, TLS 1.2.
// Are considered valid if both major and minor are in 0-9 range, and the version is not 0.0.
template <const char* Prefix>
class MajorMinorVersion {
 public:
  static constexpr std::string_view kPrefix = Prefix;
  static constexpr auto kStrLen = kPrefix.size() + (2UL * 1UL) + 1UL;

  // Constructs an empty, invalid version.
  constexpr MajorMinorVersion() noexcept = default;

  // Constructs a version with given major and minor version numbers.
  // If the numbers are not both single-digit (0-9), equivalent to default constructed.
  constexpr MajorMinorVersion(std::uint8_t majorVer, std::uint8_t minorVer) noexcept {
    if (majorVer <= 9U && minorVer <= 9U) {
      _data = static_cast<std::uint8_t>(static_cast<uint8_t>(majorVer << 4U) | minorVer);
    }
  }

  // Parses a full version string "<Prefix>X.Y" (e.g. "HTTP/1.1"), X and Y being digits.
  // Any other string (including a different case of the prefix) gives an invalid version.
  constexpr MajorMinorVersion(const char* pStr, std::size_t len) noexcept {
    if (len == kStrLen && std::string_view(pStr, kPrefix.size()) == kPrefix && pStr[kPrefix.size() + 1UL] == '.') {
      *this = MajorMinorVersion(DigitValue(pStr[kPrefix.size()]), DigitValue(pStr[kPrefix.size() + 2UL]));
    }
  }

  constexpr MajorMinorVersion(std::string_view versionStr) noexcept  // NOLINT(*-explicit-constructor)
      : MajorMinorVersion(versionStr.data(), versionStr.size()) {}

  // Parses a full version string "<Prefix><Major>.Y" (e.g. "HTTP/1.1" for Major 1), Y being a digit.
  // Any other string gives an invalid version - even a valid version of another major version, which the constructor
  // from a string tells apart from a malformed one. Faster than the latter, as "<Prefix><Major>." is matched in a
  // single comparison: use it first when a single major version is supported.
  template <std::uint8_t Major>
  [[nodiscard]] static constexpr MajorMinorVersion OfMajor(const char* pStr, std::size_t len) noexcept {
    static_assert(Major >= 1U && Major <= 9U);
    constexpr std::string_view kHead = kMajorHead<Major>;
    if (len == kStrLen && std::string_view(pStr, kHead.size()) == kHead) {
      return {Major, DigitValue(pStr[kHead.size()])};
    }
    return {};
  }

  // Get the major version number.
  [[nodiscard]] constexpr std::uint8_t major() const noexcept { return static_cast<std::uint8_t>(_data >> 4U); }

  // Get the minor version number.
  [[nodiscard]] constexpr std::uint8_t minor() const noexcept { return static_cast<std::uint8_t>(_data & 0x0FU); }

  // Returns true if the version is valid.
  [[nodiscard]] constexpr bool isValid() const noexcept { return _data != 0; }

  // Returns the full version string in a std::array<char> (e.g. "HTTP/1.1").
  [[nodiscard]] constexpr auto str() const noexcept {
    std::array<char, kStrLen> buf;
    writeFull(buf.data());
    return buf;
  }

  // Write the full version string (e.g. "HTTP/1.1") to out.
  // Returns pointer to one past the last written character.
  constexpr char* writeFull(char* out) const { return writeMajorMinor(AppendFixed<kPrefix>(out)); }

  // Write just the "X.Y" part of the version to out.
  // Returns pointer to one past the last written character.
  constexpr char* writeMajorMinor(char* out) const {
    *out++ = static_cast<char>('0' + major());
    *out++ = '.';
    *out++ = static_cast<char>('0' + minor());
    return out;
  }

  constexpr auto operator<=>(const MajorMinorVersion&) const noexcept = default;

 private:
  // Value of a digit character, or a value above 9 if ch is not a digit.
  static constexpr std::uint8_t DigitValue(char ch) noexcept { return static_cast<std::uint8_t>(ch - '0'); }

  // "<Prefix><Major>." (e.g. "HTTP/1."): a version string of major version Major, without its minor version.
  template <std::uint8_t Major>
  static constexpr std::string_view kMajorHead =
      JoinStringView_v<kPrefix, CharToStringView_v<static_cast<char>('0' + Major)>, CharToStringView_v<'.'>>;

  std::uint8_t _data{};
};

}  // namespace aeronet
