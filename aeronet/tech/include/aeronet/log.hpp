#pragma once

// Logging of aeronet: same calls as spdlog (trace() to critical(), set_level(), level::...), with format strings
// checked at compile time (aeronet/format-lite.hpp: the output and the format specifications of std::format, for bool,
// char, integers, float, double, strings and void pointers). A call costs a relaxed load and a comparison when its
// level is filtered out. Otherwise the message is written by a non-template function of src/log.cpp: through the
// default logger of spdlog (its sinks and pattern) with AERONET_ENABLE_SPDLOG, by a built-in logger writing to stdout
// without it. Neither spdlog nor <format> is included by this header.
//
// The calls never throw: a message that cannot be formatted or written is dropped, with a line on stderr.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/format-lite.hpp"

namespace aeronet::log {

namespace level {
// Names and values of spdlog::level::level_enum. A message is written when its level is at least the current one, so
// that 'off' writes none.
enum level_enum : std::uint8_t { trace, debug, info, warn, err, critical, off };
}  // namespace level

template <class... Args>
using format_string_t = FormatString<Args...>;

namespace detail {

// The current level, read by every call.
extern std::atomic<level::level_enum> g_level;

// Writes 'message' as is.
void Write(level::level_enum lvl, std::string_view message) noexcept;

// Writes 'fmt' formatted with the 'nbArgs' arguments of 'pArgs'.
void Write(level::level_enum lvl, std::string_view fmt, const FormatArg* pArgs, std::size_t nbArgs) noexcept;

template <class... Args>
void Log(level::level_enum lvl, std::string_view fmt, const Args&... args) noexcept {
  if (lvl >= g_level.load(std::memory_order_relaxed)) {
    if constexpr (sizeof...(Args) == 0) {
      Write(lvl, fmt);
    } else {
      const FormatArg formatArgs[]{FormatArg(args)...};
      Write(lvl, fmt, formatArgs, sizeof...(Args));
    }
  }
}

}  // namespace detail

/// The current level, process-wide (info by default).
[[nodiscard]] inline level::level_enum get_level() noexcept { return detail::g_level.load(std::memory_order_relaxed); }

/// Sets the level of the aeronet messages, process-wide. With spdlog, also sets the level of its loggers
/// (spdlog::set_level()): a level set through spdlog only does not change which messages aeronet writes.
void set_level(level::level_enum lvl);

/// Writes 'message' as is (its braces are not replacement fields) if 'lvl' is enabled.
inline void log(level::level_enum lvl, std::string_view message) noexcept { detail::Log(lvl, message); }

/// Writes 'fmt' formatted with the arguments if 'lvl' is enabled.
template <class Arg, class... Args>
void log(level::level_enum lvl, FormatString<Arg, Args...> fmt, const Arg& arg, const Args&... args) noexcept {
  detail::Log(lvl, fmt.get(), arg, args...);
}

inline void trace(std::string_view message) noexcept { detail::Log(level::trace, message); }

template <class Arg, class... Args>
void trace(FormatString<Arg, Args...> fmt, const Arg& arg, const Args&... args) noexcept {
  detail::Log(level::trace, fmt.get(), arg, args...);
}

inline void debug(std::string_view message) noexcept { detail::Log(level::debug, message); }

template <class Arg, class... Args>
void debug(FormatString<Arg, Args...> fmt, const Arg& arg, const Args&... args) noexcept {
  detail::Log(level::debug, fmt.get(), arg, args...);
}

inline void info(std::string_view message) noexcept { detail::Log(level::info, message); }

template <class Arg, class... Args>
void info(FormatString<Arg, Args...> fmt, const Arg& arg, const Args&... args) noexcept {
  detail::Log(level::info, fmt.get(), arg, args...);
}

inline void warn(std::string_view message) noexcept { detail::Log(level::warn, message); }

template <class Arg, class... Args>
void warn(FormatString<Arg, Args...> fmt, const Arg& arg, const Args&... args) noexcept {
  detail::Log(level::warn, fmt.get(), arg, args...);
}

inline void error(std::string_view message) noexcept { detail::Log(level::err, message); }

template <class Arg, class... Args>
void error(FormatString<Arg, Args...> fmt, const Arg& arg, const Args&... args) noexcept {
  detail::Log(level::err, fmt.get(), arg, args...);
}

inline void critical(std::string_view message) noexcept { detail::Log(level::critical, message); }

template <class Arg, class... Args>
void critical(FormatString<Arg, Args...> fmt, const Arg& arg, const Args&... args) noexcept {
  detail::Log(level::critical, fmt.get(), arg, args...);
}

}  // namespace aeronet::log
