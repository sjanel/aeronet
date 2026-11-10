#pragma once

#include "aeronet/amc-type-traits.hpp"  // IWYU pragma: keep

#define AMC_NONSTD_FEATURES
#include <amc/flatset.hpp>
#undef AMC_NONSTD_FEATURES

namespace aeronet {
using amc::FlatSet;
}  // namespace aeronet
