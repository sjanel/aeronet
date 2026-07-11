#pragma once

#include <cstdint>
#include <string_view>

namespace aeronet::internal {

struct UrlParseResult {
  [[nodiscard]] bool hasNonDefaultPort() const noexcept { return (isTls && port != 443) || (!isTls && port != 80); }

  std::string_view host;    // host without the port.
  std::string_view target;  // origin-form target; "/" when the URL carries no path
  uint16_t port{};
  bool isTls{};
};

}  // namespace aeronet::internal