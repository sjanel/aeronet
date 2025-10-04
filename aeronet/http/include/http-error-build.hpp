// Builds an in-memory HTTP/1.1 minimal error response (no body) using provided status, reason, date and connection
// header. The output string is replaced with the complete response bytes. Returns true on success (always true
// currently).
#pragma once

#include "aeronet/http-status-code.hpp"
#include "raw-chars.hpp"
#include "timedef.hpp"

namespace aeronet {

void BuildSimpleError(http::StatusCode status, TimePoint tp, RawChars &out);

}  // namespace aeronet
