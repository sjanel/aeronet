#pragma once

#include <string_view>

#include "aeronet/http-method.hpp"

namespace aeronet::http {

// Attempt to parse a HTTP method.
// RFC 9110 §9.1: The method token is case-sensitive, BUT:
// RFC 9110 §2.5. The spec encourages robustness:
// "Although methods are case-sensitive, the implementation SHOULD be case-insensitive when parsing received messages.”
// Returns kMethodInvalid if the method is not recognized.
Method ParseMethodStr(std::string_view str);

}  // namespace aeronet::http