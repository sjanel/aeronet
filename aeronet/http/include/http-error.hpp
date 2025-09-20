#pragma once

#include <string_view>

namespace aeronet {

// Send a minimal HTTP error response (no body) with Date and Connection headers.
// closeConn decides the Connection header value and whether caller should close afterwards.
// Returns true if full header bytes were written.
bool sendSimpleError(int fd, int status, std::string_view reason, std::string_view date, bool closeConn);

}  // namespace aeronet
