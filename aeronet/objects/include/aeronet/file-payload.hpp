#pragma once

#include <amc/type_traits.hpp>
#include <cstddef>

#include "aeronet/file.hpp"

namespace aeronet {

struct FilePayload {
  using trivially_relocatable = amc::is_trivially_relocatable<File>::type;

  File file;
  std::size_t offset{0};
  std::size_t length{0};
};

}  // namespace aeronet