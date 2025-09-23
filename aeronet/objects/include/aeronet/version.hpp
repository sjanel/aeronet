#pragma once

#include <string_view>

namespace aeronet {

constexpr std::string_view version() { return AERONET_PROJECT_VERSION; }

}  // namespace aeronet