#pragma once

#include <cstdint>
#include <string_view>

#include "fixedcapacityvector.hpp"
#include "nchars.hpp"

namespace aeronet::http {

// RFC 9112 §2.5 HTTP version token representation
struct Version {
  static constexpr std::string_view kPrefix = "HTTP/";
  using VersionInt = uint8_t;
  using VersionStr =
      FixedCapacityVector<char, kPrefix.size() + (2UL * nchars(std::numeric_limits<VersionInt>::max())) + 1UL>;

  VersionInt major{};
  VersionInt minor{};

  // Builds a vector-like representation of the version as a string.
  [[nodiscard]] VersionStr str() const;

  constexpr auto operator<=>(const Version &) const noexcept = default;
};

// Canonical constants for supported versions (extend here if adding HTTP/2, etc.).
inline constexpr Version HTTP_0_9{0, 9};
inline constexpr Version HTTP_1_0{1, 0};
inline constexpr Version HTTP_1_1{1, 1};

// Parse a textual HTTP version token (e.g. "HTTP/1.1") into Version.
// Returns true on success; false if format invalid.
bool parseHttpVersion(const char *first, const char *last, Version &out);

}  // namespace aeronet::http
