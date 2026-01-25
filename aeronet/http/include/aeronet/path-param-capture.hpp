#pragma once

#include <string_view>

namespace aeronet {

struct PathParamCapture {
  std::string_view key;
  std::string_view value;
};

}  // namespace aeronet