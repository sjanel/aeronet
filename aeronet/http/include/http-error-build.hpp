// Builds an in-memory HTTP/1.1 minimal error response (no body) using provided status, reason, date and connection
// header. The output string is replaced with the complete response bytes. Returns true on success (always true
// currently).
#pragma once

#include <string_view>

#include "http-constants.hpp"
#include "string.hpp"

namespace aeronet {

string buildSimpleError(http::StatusCode status, std::string_view reason, std::string_view date, bool closeConn);

}  // namespace aeronet
