#pragma once

#include <cstddef>

#include "aeronet/file.hpp"

namespace aeronet {

struct FilePayload {
  File file;
  std::size_t offset{0};
  std::size_t length{0};
};

}  // namespace aeronet