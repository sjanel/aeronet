#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

#include "http-constants.hpp"

namespace aeronet::http {

// Order defines bit positions elsewhere (see http-method-build.hpp)
enum class Method : uint8_t { GET, HEAD, POST, PUT, DELETE, CONNECT, OPTIONS, TRACE, PATCH };

inline constexpr auto kNbMethods = 9;

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

}  // namespace aeronet::http