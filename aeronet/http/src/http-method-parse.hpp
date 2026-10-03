#pragma once

#include <string_view>

#include "aeronet/http-method.hpp"

namespace aeronet::http {

// Attempt to parse a HTTP method.
// RFC 9110 §9.1: The method token is case-sensitive, so "get" is not "GET".
// Returns kMethodInvalid if the method is not recognized.
Method ParseMethodStr(std::string_view str);

}  // namespace aeronet::http