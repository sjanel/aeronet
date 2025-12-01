#pragma once

#include <span>
#include <string>

namespace aeronet {

[[nodiscard]] std::string B64Decode(std::span<const char> ascData);
std::string B64Decode(const char *) = delete;

}  // namespace aeronet
