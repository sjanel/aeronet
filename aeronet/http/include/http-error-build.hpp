// Builds an in-memory HTTP/1.1 minimal error response (no body) using provided status, reason, date and connection
// header. The output string is replaced with the complete response bytes. Returns true on success (always true
// currently).
#pragma once

#include <string_view>

#include "aeronet/http-status-code.hpp"
#include "raw-chars.hpp"

namespace aeronet {

RawChars BuildSimpleError(http::StatusCode status, std::string_view date, bool closeConnection);

}  // namespace aeronet
