#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace aeronet::http {

enum class Method : uint16_t {
  GET = 1 << 0,
  HEAD = 1 << 1,
  POST = 1 << 2,
  PUT = 1 << 3,
  DELETE = 1 << 4,
  CONNECT = 1 << 5,
  OPTIONS = 1 << 6,
  TRACE = 1 << 7,
  PATCH = 1 << 8
};

using MethodIdx = std::underlying_type_t<Method>;
inline constexpr MethodIdx kNbMethods = 9;

using MethodBmp = uint16_t;

constexpr MethodBmp operator|(Method lhs, Method rhs) noexcept {
  using T = std::underlying_type_t<Method>;
  return static_cast<MethodBmp>(static_cast<T>(lhs) | static_cast<T>(rhs));
}

constexpr MethodBmp operator|(MethodBmp lhs, Method rhs) noexcept {
  using T = std::underlying_type_t<Method>;
  return static_cast<MethodBmp>(static_cast<T>(lhs) | static_cast<T>(rhs));
}

constexpr MethodBmp operator|(Method lhs, MethodBmp rhs) noexcept {
  using T = std::underlying_type_t<Method>;
  return static_cast<MethodBmp>(static_cast<T>(lhs) | static_cast<T>(rhs));
}

static_assert(kNbMethods <= sizeof(MethodBmp) * 8,
              "MethodBmp type too small to hold all methods; increase size or change type");

// Check if a method is allowed by mask.
constexpr bool IsMethodSet(MethodBmp mask, Method method) { return (mask & static_cast<MethodBmp>(method)) != 0U; }

constexpr bool IsMethodSet(MethodBmp mask, MethodIdx methodIdx) {
  return (mask & (1U << static_cast<MethodBmp>(methodIdx))) != 0U;
}

constexpr MethodIdx MethodToIdx(Method method) {
  return static_cast<MethodIdx>(std::countr_zero(static_cast<MethodIdx>(method)));
}

constexpr Method MethodFromIdx(MethodIdx methodIdx) { return static_cast<http::Method>(1U << methodIdx); }

inline constexpr std::string_view kMethodStrings[] = {"GET",     "HEAD",    "POST",  "PUT",  "DELETE",
                                                      "CONNECT", "OPTIONS", "TRACE", "PATCH"};

constexpr std::string_view MethodIdxToStr(Method methodIdx) { return kMethodStrings[MethodToIdx(methodIdx)]; }

inline constexpr std::size_t kAllMethodsStrLen = []() {
  std::size_t len = 0;
  for (MethodIdx methodIdx = 0; methodIdx < kNbMethods; ++methodIdx) {
    len += MethodIdxToStr(MethodFromIdx(methodIdx)).size();
  }
  return len;
}();

}  // namespace aeronet::http