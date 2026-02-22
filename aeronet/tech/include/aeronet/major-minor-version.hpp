#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "aeronet/memory-utils.hpp"

namespace aeronet {

// Single digit Major.Minor version representation, e.g. HTTP/1.1, TLS 1.2.
// Are considered valid if both major and minor are in 0-9 range, and major != 0.
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
    if (majorVer > 0 && majorVer <= 9 && minorVer <= 9) {
      _data = static_cast<std::uint8_t>((majorVer << 4U) | minorVer);
    }
  }

  constexpr MajorMinorVersion(std::string_view versionStr) noexcept {
    if (versionStr.size() == kStrLen) {
      const char* pStr = versionStr.data();
      char major = pStr[kPrefix.size()];
      char minor = pStr[kPrefix.size() + 2UL];
      if (major >= '1' && major <= '9' && pStr[kPrefix.size() + 1UL] == '.' && minor >= '0' && minor <= '9') {
        _data = static_cast<std::uint8_t>((major - '0') << 4U) | static_cast<std::uint8_t>(minor - '0');
      }
    }
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
  constexpr char* writeFull(char* out) const { return writeMajorMinor(Append(kPrefix, out)); }

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
  std::uint8_t _data{};
};

}  // namespace aeronet
