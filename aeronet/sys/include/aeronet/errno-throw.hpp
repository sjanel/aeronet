#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/format-lite.hpp"
#include "aeronet/system-error.hpp"

namespace aeronet {

namespace detail {

// Throws std::system_error for 'err' with 'fmt' formatted with the 'nbArgs' arguments of 'pArgs' (the non-template
// part of ThrowSystemError).
[[noreturn]] void ThrowFormattedSystemError(int err, std::string_view fmt, const FormatArg* pArgs, std::size_t nbArgs);

}  // namespace detail

// Throw std::system_error for the system/socket error code 'err' (errno value on POSIX, WSAGetLastError() code on
// Windows), with the appropriate error-code category for the platform, and a message checked at compile time
// (aeronet/format-lite.hpp).
// Use when the error was captured before calls that may overwrite the last error (logging, cleanup...).
// Usage: ThrowSystemError(err, "bind failed on port {}", port);
template <class... Args>
[[noreturn]] void ThrowSystemError(int err, FormatString<Args...> fmt, const Args&... args) {
  const FormatArg formatArgs[]{FormatArg(args)..., FormatArg()};
  detail::ThrowFormattedSystemError(err, fmt.get(), formatArgs, sizeof...(Args));
}

// Capture the last system/socket error and throw std::system_error.
// Uses LastSystemError() (errno on POSIX, WSAGetLastError() on Windows)
// and the appropriate error-code category for the platform.
// Use for socket / OS-level failures.
// Usage: ThrowSystemError("bind failed on port {}", port);
template <class... Args>
[[noreturn]] void ThrowSystemError(FormatString<Args...> fmt, const Args&... args) {
  const int err = LastSystemError();
  const FormatArg formatArgs[]{FormatArg(args)..., FormatArg()};
  detail::ThrowFormattedSystemError(err, fmt.get(), formatArgs, sizeof...(Args));
}

}  // namespace aeronet
