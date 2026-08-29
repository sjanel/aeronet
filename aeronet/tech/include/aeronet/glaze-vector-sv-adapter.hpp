#pragma once

#include <string_view>

#include "aeronet/vector.hpp"

namespace aeronet::glz_detail {

// Adapts a `const T&`-accessor that returns a range of string-like elements
// into aeronet::vector<std::string_view>, for use as a glz::custom getter.
template <auto MemberFn>
constexpr auto ToStringViewVector = [](const auto& self) {
  aeronet::vector<std::string_view> result;
  for (auto sv : (self.*MemberFn)()) {
    result.push_back(sv);
  }
  return result;
};

}  // namespace aeronet::glz_detail