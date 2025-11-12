#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <system_error>
#include <utility>

#include "aeronet/fixedcapacityvector.hpp"
#include "aeronet/nchars.hpp"
#include "aeronet/stringconv.hpp"

namespace aeronet {

template <const char *Prefix, class VersionInt = uint8_t>
struct MajorMinorVersion {
  static constexpr std::string_view kPrefix = Prefix;
  static constexpr std::size_t kMinStrLen = kPrefix.size() + (2UL * 1UL) + 1UL;
  static constexpr std::size_t kMaxStrLen =
      kPrefix.size() + (2UL * nchars(std::numeric_limits<VersionInt>::max())) + 1UL;

  using VersionStr = FixedCapacityVector<char, kMaxStrLen>;

  VersionInt major{};
  VersionInt minor{};

  // Builds a vector-like representation of the version as a string.
  // You can wrap it in a std::string_view to make it printable.
  [[nodiscard]] VersionStr str() const {
    VersionStr ret;
    ret.append_range(kPrefix);
    ret.append_range(IntegralToCharVector(major));
    ret.push_back('.');
    ret.append_range(IntegralToCharVector(minor));
    return ret;
  }

  constexpr auto operator<=>(const MajorMinorVersion &) const noexcept = default;
};

// Parse a textual version token (e.g. "HTTP/1.1") into Version.
// Returns true on success; false if format invalid.
template <const char *Prefix, class VersionInt>
bool parseVersion(const char *first, const char *last, MajorMinorVersion<Prefix, VersionInt> &out) {
  using T = MajorMinorVersion<Prefix, VersionInt>;
  if (std::cmp_less(last - first, T::kMinStrLen) || std::memcmp(first, T::kPrefix.data(), T::kPrefix.size()) != 0) {
    return false;
  }
  first += T::kPrefix.size();

  const auto dot = static_cast<const char *>(std::memchr(first, '.', static_cast<std::size_t>(last - first)));
  if (dot == nullptr) {
    return false;
  }

  return std::from_chars(first, dot, out.major).ec == std::errc() &&
         std::from_chars(dot + 1, last, out.minor).ec == std::errc();
}

}  // namespace aeronet
