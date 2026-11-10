#pragma once

#include <format>
#include <string_view>
#include <system_error>
#include <utility>

#include "aeronet/system-error.hpp"

namespace aeronet {

// Throw std::system_error for the system/socket error code 'err' (errno value on POSIX, WSAGetLastError() code on
// Windows), with the appropriate error-code category for the platform.
// Use when the error was captured before calls that may overwrite the last error (logging, cleanup...).
// Usage: ThrowSystemError(err, "bind failed on port {}", port);
template <typename... Args>
[[noreturn]] void ThrowSystemError(int err, std::string_view fmt, Args&&... args) {
#ifdef AERONET_WINDOWS
  std::error_code ec(err, std::system_category());
#else
  std::error_code ec(err, std::generic_category());
#endif
  // std::make_format_args takes lvalues only: args (temporaries included) are named here.
  throw std::system_error(ec, std::vformat(fmt, std::make_format_args(args...)));
}

// Capture the last system/socket error and throw std::system_error.
// Uses LastSystemError() (errno on POSIX, WSAGetLastError() on Windows)
// and the appropriate error-code category for the platform.
// Use for socket / OS-level failures.
// Usage: ThrowSystemError("bind failed on port {}", port);
template <typename... Args>
[[noreturn]] void ThrowSystemError(std::string_view fmt, Args&&... args) {
  ThrowSystemError(LastSystemError(), fmt, std::forward<Args>(args)...);
}

}  // namespace aeronet
