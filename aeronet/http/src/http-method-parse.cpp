#include "http-method-parse.hpp"

#include <string_view>

#include "aeronet/http-method.hpp"

namespace aeronet::http {

Method ParseMethodStr(std::string_view str) {
  // Compilers fully unroll this loop over the constexpr table into a dispatch on the length followed by one or two
  // inlined integer comparisons - measured as fast as (or faster than) a hand-written switch with clang and gcc.
  for (MethodIdx methodIdx = 0; methodIdx < kNbMethods; ++methodIdx) {
    if (str == kMethodStrings[methodIdx]) {
      return MethodFromIdx(methodIdx);
    }
  }
  return kMethodInvalid;
}

}  // namespace aeronet::http
