#pragma once

#include <cerrno>
#include <format>
#include <string_view>
#include <system_error>

#include "aeronet/system-error.hpp"

namespace aeronet {

// Capture the last system/socket error and throw std::system_error.
// Uses LastSystemError() (errno on POSIX, WSAGetLastError() on Windows)
// and the appropriate error-code category for the platform.
// Use for socket / OS-level failures.
// Usage: ThrowSystemError("bind failed on port {}", port);
template <typename... Args>
[[noreturn]] void ThrowSystemError(std::string_view fmt, Args&&... args) {
  const int savedErr = LastSystemError();
#ifdef AERONET_WINDOWS
  std::error_code ec(savedErr, std::system_category());
#else
  std::error_code ec(savedErr, std::generic_category());
#endif
  throw std::system_error(ec, std::vformat(fmt, std::make_format_args(std::forward<Args>(args)...)));
}

}  // namespace aeronet
