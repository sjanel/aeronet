#pragma once

// Logging abstraction: spdlog when available; otherwise a lightweight fallback.
#ifdef AERONET_ENABLE_SPDLOG
// Ensure header-only usage is forced locally without exporting SPDLOG_HEADER_ONLY
// as a public compile definition (avoids redefinition warnings if consumers also
// decide to force header-only or use the compiled lib variant).
#ifndef SPDLOG_HEADER_ONLY
#define SPDLOG_HEADER_ONLY
#endif
#include <spdlog/common.h>  // IWYU pragma: export
#include <spdlog/spdlog.h>  // IWYU pragma: export
#else
#include <format>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>

#include "timedef.hpp"
#include "timestring.hpp"
#endif

namespace aeronet {
#ifdef AERONET_ENABLE_SPDLOG
namespace log = spdlog;
#else
namespace log {
// Fallback logger with runtime formatting (std::vformat) and graceful degradation.
struct level {
  using level_enum = int;
  static constexpr int trace = 0;
  static constexpr int debug = 1;
  static constexpr int info = 2;
  static constexpr int warn = 3;
  static constexpr int err = 4;
  static constexpr int critical = 5;
  static constexpr int off = 6;
};

static constexpr const char *kLevelNames[] = {
    "trace", "debug", "info", "warn", "error", "critical", "off",
};

inline level::level_enum &current_level() {
  static level::level_enum lvl = level::info;  // default verbosity (info and above)
  return lvl;
}

inline void set_level(level::level_enum lvl) { current_level() = lvl; }
inline level::level_enum get_level() { return current_level(); }

namespace detail {
template <typename... Args>
std::string run_format(std::string_view fmt, Args &&...args) {
  if constexpr (sizeof...(Args) == 0) {
    return std::string(fmt);
  } else {
    return std::vformat(fmt, std::make_format_args(args...));
  }
}

inline void emit_line(const char *lvlTag, bool isErr, std::string_view msg) {
  auto &os = isErr ? std::cerr : std::cout;
  char timeBuf[25];
  *TimeToStringISO8601UTCWithMs(SysClock::now(), timeBuf) = '\0';
  os << '[' << timeBuf << "] [" << lvlTag << "] " << msg << '\n';
}
}  // namespace detail

template <typename... Args>
void log(level::level_enum level, std::string_view fmt, Args &&...args) {
  if (static_cast<int>(get_level()) <= static_cast<int>(level)) {
    detail::emit_line(kLevelNames[static_cast<int>(level)], false,
                      detail::run_format(fmt, std::forward<Args>(args)...));
  }
}

template <typename... Args>
void trace(std::string_view fmt, Args &&...args) {
  log(level::trace, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void debug(std::string_view fmt, Args &&...args) {
  log(level::debug, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void info(std::string_view fmt, Args &&...args) {
  log(level::info, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void warn(std::string_view fmt, Args &&...args) {
  log(level::warn, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void error(std::string_view fmt, Args &&...args) {
  log(level::err, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void critical(std::string_view fmt, Args &&...args) {
  log(level::critical, fmt, std::forward<Args>(args)...);
}

}  // namespace log
#endif

}  // namespace aeronet
