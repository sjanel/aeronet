#pragma once

#include <string_view>

#include "flat-hash-map.hpp"

namespace aeronet {

using HttpHeaders = flat_hash_map<std::string_view, std::string_view>;

struct HttpRequest {
  std::string_view method;
  std::string_view target;
  std::string_view version;
  HttpHeaders headers;
  std::string_view body;

  [[nodiscard]] std::string_view findHeader(std::string_view key) const;
};

}  // namespace aeronet