#pragma once

#include <chrono>
#include <cstddef>
#include <string_view>

#include "aeronet/http-method.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"

namespace aeronet {

struct RequestMetrics {
  http::StatusCode status{0};
  http::Method method;
  http::Version version;
  bool reusedConnection{false};
  std::string_view path;
  std::string_view clientIp;
  std::string_view userAgent;
  std::size_t bytesIn{0};
  std::size_t bytesOut{0};
  std::chrono::nanoseconds duration{0};
};

}  // namespace aeronet
