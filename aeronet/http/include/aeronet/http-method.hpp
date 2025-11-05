#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "aeronet/http-constants.hpp"

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
constexpr bool isMethodSet(MethodBmp mask, Method method) { return (mask & static_cast<MethodBmp>(method)) != 0U; }

constexpr bool isMethodSet(MethodBmp mask, MethodIdx methodIdx) {
  return (mask & (1U << static_cast<MethodBmp>(methodIdx))) != 0U;
}

constexpr MethodIdx toMethodIdx(Method method) {
  switch (method) {
    case Method::GET:
      return 0;
    case Method::HEAD:
      return 1;
    case Method::POST:
      return 2;
    case Method::PUT:
      return 3;
    case Method::DELETE:
      return 4;
    case Method::CONNECT:
      return 5;
    case Method::OPTIONS:
      return 6;
    case Method::TRACE:
      return 7;
    case Method::PATCH:
      return 8;
    default:
      std::unreachable();
  }
}

constexpr Method fromMethodIdx(MethodIdx idx) {
  switch (idx) {
    case 0:
      return Method::GET;
    case 1:
      return Method::HEAD;
    case 2:
      return Method::POST;
    case 3:
      return Method::PUT;
    case 4:
      return Method::DELETE;
    case 5:
      return Method::CONNECT;
    case 6:
      return Method::OPTIONS;
    case 7:
      return Method::TRACE;
    case 8:
      return Method::PATCH;
    default:
      std::unreachable();
  }
}

// constexpr mapping from method token to enum; falls back to GET if unknown.
constexpr std::optional<Method> toMethodEnum(std::string_view methodStr) {
  if (methodStr == http::GET) {
    return Method::GET;
  }
  if (methodStr == http::HEAD) {
    return Method::HEAD;
  }
  if (methodStr == http::POST) {
    return Method::POST;
  }
  if (methodStr == http::PUT) {
    return Method::PUT;
  }
  if (methodStr == http::DELETE) {
    return Method::DELETE;
  }
  if (methodStr == http::CONNECT) {
    return Method::CONNECT;
  }
  if (methodStr == http::OPTIONS) {
    return Method::OPTIONS;
  }
  if (methodStr == http::TRACE) {
    return Method::TRACE;
  }
  if (methodStr == http::PATCH) {
    return Method::PATCH;
  }
  return {};
}

constexpr std::string_view toMethodStr(Method method) {
  switch (method) {
    case Method::GET:
      return http::GET;
    case Method::HEAD:
      return http::HEAD;
    case Method::POST:
      return http::POST;
    case Method::PUT:
      return http::PUT;
    case Method::DELETE:
      return http::DELETE;
    case Method::CONNECT:
      return http::CONNECT;
    case Method::OPTIONS:
      return http::OPTIONS;
    case Method::TRACE:
      return http::TRACE;
    case Method::PATCH:
      return http::PATCH;
    default:
      std::unreachable();
  }
}

inline constexpr std::size_t kAllMethodsStrLen = []() {
  std::size_t len = 0;
  for (MethodIdx methodIdx = 0; methodIdx < kNbMethods; ++methodIdx) {
    len += toMethodStr(fromMethodIdx(methodIdx)).size();
  }
  return len;
}();

}  // namespace aeronet::http