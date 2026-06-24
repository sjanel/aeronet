#pragma once

#include <cstdio>
#include <string_view>
#include <utility>

#include "aeronet/log.hpp"

namespace aeronet::log_noexcept {

using level_t = aeronet::log::level::level_enum;

// Format-string type accepted by the wrappers below. It must match the type the
// underlying logger expects so that the (consteval) compile-time format check
// happens at the call site, where the literal is a constant expression.
//
// The previous design took the format string through a forwarding reference
// (Format&&) and forwarded it into the logger. With std::format-backed spdlog
// (SPDLOG_USE_STD_FORMAT, as on macOS/Windows) the logger expects
// std::format_string<Args...>, whose constructor is consteval: a function
// parameter is never a constant expression, so the conversion failed to compile.
#ifdef AERONET_ENABLE_SPDLOG
template <typename... Args>
using format_string_t = aeronet::log::format_string_t<Args...>;
#else
template <typename... Args>
using format_string_t = std::string_view;
#endif

// Use these wrappers only from noexcept paths (e.g. destructors). They keep the
// usual logger API while ensuring formatting/backend exceptions never escape.
template <typename... Args>
void log(level_t level, format_string_t<Args...> fmt, Args&&... args) noexcept {
  try {
    aeronet::log::log(level, fmt, std::forward<Args>(args)...);
  } catch (...) {
    std::fputs("aeronet::log_noexcept::log failed\n", stderr);
  }
}

#define AERONET_NOEXCEPT_LOG_WRAPPER(name, level)                    \
  template <typename... Args>                                        \
  void name(format_string_t<Args...> fmt, Args&&... args) noexcept { \
    log(level, fmt, std::forward<Args>(args)...);                    \
  }

AERONET_NOEXCEPT_LOG_WRAPPER(trace, aeronet::log::level::trace)
AERONET_NOEXCEPT_LOG_WRAPPER(debug, aeronet::log::level::debug)
AERONET_NOEXCEPT_LOG_WRAPPER(info, aeronet::log::level::info)
AERONET_NOEXCEPT_LOG_WRAPPER(warn, aeronet::log::level::warn)
AERONET_NOEXCEPT_LOG_WRAPPER(error, aeronet::log::level::err)
AERONET_NOEXCEPT_LOG_WRAPPER(critical, aeronet::log::level::critical)

#undef AERONET_NOEXCEPT_LOG_WRAPPER

}  // namespace aeronet::log_noexcept
