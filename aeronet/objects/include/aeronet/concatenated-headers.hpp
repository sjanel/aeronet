#pragma once

#include <cstdint>

#include "aeronet/dynamic-concatenated-strings.hpp"

namespace aeronet {

namespace detail {
inline constexpr char kCRLFChars[] = {"\r\n"};
}

using ConcatenatedHeaders = DynamicConcatenatedStrings<detail::kCRLFChars, uint32_t>;

enum class HeaderType : uint8_t {
  Request,
  Response,
};

void Validate(const ConcatenatedHeaders& headers, HeaderType type);

}  // namespace aeronet