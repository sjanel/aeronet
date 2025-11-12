#pragma once

#include <cstdint>

#include "aeronet/major-minor-version.hpp"

namespace aeronet::http {

// RFC 9112 §2.5 HTTP version token representation
inline constexpr char kHttpPrefix[] = "HTTP/";

using Version = MajorMinorVersion<kHttpPrefix, uint8_t>;

// Canonical constants for supported versions (extend here if adding HTTP/2, etc.).
inline constexpr Version HTTP_0_9{0, 9};
inline constexpr Version HTTP_1_0{1, 0};
inline constexpr Version HTTP_1_1{1, 1};

}  // namespace aeronet::http
