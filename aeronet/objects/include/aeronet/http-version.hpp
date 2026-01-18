#pragma once

#include "aeronet/major-minor-version.hpp"

namespace aeronet::http {

// RFC 9112 §2.5 HTTP version token representation
inline constexpr char kHttpPrefix[] = "HTTP/";

using Version = MajorMinorVersion<kHttpPrefix>;

// Canonical constants for supported versions (extend here if adding HTTP/2, etc.).
inline constexpr Version HTTP_1_0{1, 0};
inline constexpr Version HTTP_1_1{1, 1};
inline constexpr Version HTTP_2_0{2, 0};

}  // namespace aeronet::http
