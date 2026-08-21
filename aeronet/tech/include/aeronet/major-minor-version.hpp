#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "aeronet/memory-utils-sv.hpp"

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

  constexpr MajorMinorVersion(const char* pStr, std::size_t len) noexcept {
    if (len == kStrLen) {
      const char major = pStr[kPrefix.size()];
      const char minor = pStr[kPrefix.size() + 2UL];
      if (major >= '1' && major <= '9' && pStr[kPrefix.size() + 1UL] == '.' && minor >= '0' && minor <= '9') {
        _data = static_cast<std::uint8_t>(static_cast<std::uint8_t>(static_cast<std::uint8_t>(major - '0') << 4U) |
                                          static_cast<std::uint8_t>(minor - '0'));
      }
    }
  }

  constexpr MajorMinorVersion(std::string_view versionStr) noexcept
      : MajorMinorVersion(versionStr.data(), versionStr.size()) {}

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
  std::uint8_t _data{};
};

}  // namespace aeronet

#ifdef AERONET_ENABLE_GLAZE

#include <glaze/glaze.hpp>
#include <string>

// --- MajorMinorVersion<Prefix>: read/write as string like "1.2" ---
template <const char* Prefix>
struct glz::meta<aeronet::MajorMinorVersion<Prefix>> {
  using T = aeronet::MajorMinorVersion<Prefix>;
  static constexpr bool custom_read = true;
  static constexpr bool custom_write = true;
};

template <uint32_t Format, const char* Prefix>
struct glz::from<Format, aeronet::MajorMinorVersion<Prefix>> {
  template <auto Opts>
  static void op(aeronet::MajorMinorVersion<Prefix>& value, is_context auto&& ctx, auto&& it, auto&& end) {
    std::string str;
    from<Format, std::string>::template op<Opts>(str, ctx, it, end);
    assert(!bool(ctx.error));  // Glaze validated structure before calling custom reader
    if (str.empty()) {
      value = aeronet::MajorMinorVersion<Prefix>{};
      return;
    }
    // Accept "X.Y" (short form) or "PrefixX.Y" (full form)
    static constexpr std::string_view prefix = Prefix;
    if (str.size() == 3 && str[1] == '.') {
      // Short form "X.Y" - construct by prepending the prefix
      std::string full{prefix};
      full += str;
      value = aeronet::MajorMinorVersion<Prefix>{std::string_view{full}};
    } else {
      value = aeronet::MajorMinorVersion<Prefix>{std::string_view{str}};
    }
    if (!value.isValid() && !str.empty()) {
      ctx.error = error_code::parse_error;
    }
  }
};

template <uint32_t Format, const char* Prefix>
struct glz::to<Format, aeronet::MajorMinorVersion<Prefix>> {
  template <auto Opts, is_context Ctx, class B, class IX>
  static void op(const aeronet::MajorMinorVersion<Prefix>& value, Ctx&& ctx, B&& b, IX&& ix) {
    if (value.isValid()) {
      // Write as short form "X.Y"
      const char buf[]{static_cast<char>('0' + value.major()), '.', static_cast<char>('0' + value.minor())};
      serialize<Format>::template op<Opts>(std::string_view{buf, sizeof(buf)}, ctx, b, ix);
    } else {
      serialize<Format>::template op<Opts>(std::string_view{""}, ctx, b, ix);
    }
  }
};

#endif
