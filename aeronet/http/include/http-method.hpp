#pragma once

#include <cstdint>
#include <string_view>

namespace aeronet::http {

// Order defines bit positions elsewhere (see http-method-build.hpp)
enum class Method : uint8_t { GET, HEAD, POST, PUT, DELETE, CONNECT, OPTIONS, TRACE, PATCH };

// constexpr mapping from method token to enum; falls back to GET if unknown.
constexpr Method toMethodEnum(std::string_view methodStr) {
  if (methodStr == "GET") {
    return Method::GET;
  }
  if (methodStr == "HEAD") {
    return Method::HEAD;
  }
  if (methodStr == "POST") {
    return Method::POST;
  }
  if (methodStr == "PUT") {
    return Method::PUT;
  }
  if (methodStr == "DELETE") {
    return Method::DELETE;
  }
  if (methodStr == "CONNECT") {
    return Method::CONNECT;
  }
  if (methodStr == "OPTIONS") {
    return Method::OPTIONS;
  }
  if (methodStr == "TRACE") {
    return Method::TRACE;
  }
  if (methodStr == "PATCH") {
    return Method::PATCH;
  }
  return Method::GET;  // fallback
}

}  // namespace aeronet::http