#pragma once

#include <cerrno>
#include <format>
#include <string_view>
#include <system_error>

namespace aeronet {

// Capture errno immediately and throw std::system_error with a formatted message.
// Usage: throw_errno("bind failed for {}", path);
template <typename... Args>
[[noreturn]] void throw_errno(std::string_view fmt, Args&&... args) {
  const int savedErr = errno;
  std::error_code ec(savedErr, std::generic_category());
  // std::format may throw std::format_error; allow it to propagate — it's acceptable
  // because this is typically called during unrecoverable system failures.
  throw std::system_error(ec, std::vformat(fmt, std::make_format_args(std::forward<Args>(args)...)));
}

}  // namespace aeronet
