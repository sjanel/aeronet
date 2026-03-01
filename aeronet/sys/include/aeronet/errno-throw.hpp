#pragma once

#include <cerrno>
#include <format>
#include <string_view>
#include <system_error>

#include "aeronet/platform.hpp"

namespace aeronet {

// Capture errno immediately and throw std::system_error with a formatted message.
// Use for CRT / file-I/O failures where errno is the authoritative error source.
// Usage: throw_errno("open failed for {}", path);
template <typename... Args>
[[noreturn]] void throw_errno(std::string_view fmt, Args&&... args) {
  const int savedErr = errno;
  std::error_code ec(savedErr, std::generic_category());
  // std::format may throw std::format_error; allow it to propagate — it's acceptable
  // because this is typically called during unrecoverable system failures.
  // Note: std::make_format_args requires lvalue references in C++23, so we do not forward.
  throw std::system_error(ec, std::vformat(fmt, std::make_format_args(args...)));
}

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
  throw std::system_error(ec, std::vformat(fmt, std::make_format_args(args...)));
}

}  // namespace aeronet
