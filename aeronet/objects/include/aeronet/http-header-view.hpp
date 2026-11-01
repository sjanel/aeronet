#pragma once

#include <string_view>

namespace aeronet::http {

struct HeaderView {
  std::string_view name;
  std::string_view value;
};

}  // namespace aeronet::http