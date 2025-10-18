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
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>

#include "time-format.hpp"  // for TimePointISO8601UTC formatter
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

inline int &current_level() {
  static int lvl = level::info;  // default verbosity (info and above)
  return lvl;
}

inline void set_level(level::level_enum lvl) { current_level() = static_cast<int>(lvl); }
inline int get_level() { return current_level(); }

namespace detail {
inline std::mutex &logger_mutex() {
  static std::mutex loggerMutex;
  return loggerMutex;
}

inline std::string build_timestamp() {
  // Millisecond precision ISO 8601 UTC using the standardized formatter we already expose.
  auto now = TimePointISO8601UTC{Clock::now()};
  return std::format("{:ms}", now);
}

template <typename... Args>
std::string run_format(std::string_view fmt, Args &&...args) {
  if constexpr (sizeof...(Args) == 0) {
    return std::string(fmt);
  } else {
    return std::vformat(fmt, std::make_format_args(args...));
  }
}

inline void emit_line(const char *lvlTag, bool isErr, std::string_view msg) {
  std::lock_guard<std::mutex> lock(logger_mutex());
  auto &os = isErr ? std::cerr : std::cout;
  // Note: Allocates a small string each call; acceptable for fallback path. If needed, optimize with
  // std::format_to into a fixed-size stack buffer or a thread_local reusable string.
  os << build_timestamp() << ' ' << lvlTag << ' ' << msg << '\n';
}
}  // namespace detail

template <typename... Args>
inline void trace(std::string_view fmt, Args &&...args) {
  if (get_level() > level::trace) {
    return;
  }
  detail::emit_line("[trace]", false, detail::run_format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
inline void debug(std::string_view fmt, Args &&...args) {
  if (get_level() > level::debug) {
    return;
  }
  detail::emit_line("[debug]", false, detail::run_format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
inline void info(std::string_view fmt, Args &&...args) {
  if (get_level() > level::info) {
    return;
  }
  detail::emit_line("[info]", false, detail::run_format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
inline void warn(std::string_view fmt, Args &&...args) {
  if (get_level() > level::warn) {
    return;
  }
  detail::emit_line("[warn]", false, detail::run_format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
inline void error(std::string_view fmt, Args &&...args) {
  if (get_level() > level::err) {
    return;
  }
  detail::emit_line("[error]", true, detail::run_format(fmt, std::forward<Args>(args)...));
}
template <typename... Args>
inline void critical(std::string_view fmt, Args &&...args) {
  if (get_level() > level::critical) {
    return;
  }
  detail::emit_line("[critical]", true, detail::run_format(fmt, std::forward<Args>(args)...));
}

}  // namespace log
#endif

}  // namespace aeronet
