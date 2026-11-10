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
#include <cstddef>
#include <cstdio>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"
#endif

namespace aeronet {

#ifdef AERONET_ENABLE_SPDLOG
namespace log = spdlog;  // NOLINT(misc-unused-alias-decls)
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

static constexpr const char* kLevelNames[]{
    "trace", "debug", "info", "warn", "error", "critical", "off",
};

inline level::level_enum& current_level() {
  static level::level_enum lvl = level::info;  // default verbosity (info and above)
  return lvl;
}

inline void set_level(level::level_enum lvl) { current_level() = lvl; }
inline level::level_enum get_level() { return current_level(); }

namespace detail {

// A line '[time] [level] ', with room for the message of 'messageSize' bytes and the final new line.
inline std::string StartLine(level::level_enum lvl, std::size_t messageSize) {
  static constexpr std::string_view kTimeLevelSeparator = "] [";
  static constexpr std::string_view kLevelMessageSeparator = "] ";
  const std::string_view levelName = kLevelNames[lvl];

  std::string line;
  line.reserve(1U + ISO8601UTCWithMsStrLen + kTimeLevelSeparator.size() + levelName.size() +
               kLevelMessageSeparator.size() + messageSize + 1U);
  line.push_back('[');
  char timeBuf[ISO8601UTCWithMsStrLen];
  line.append(timeBuf, TimeToStringISO8601UTCWithMs(SysClock::now(), timeBuf));
  line.append(kTimeLevelSeparator);
  line.append(levelName);
  line.append(kLevelMessageSeparator);
  return line;
}

// Writes the line with a single call, which the C library serializes with the other writes to stdout: the lines logged
// by concurrent threads do not interleave.
inline void EndLine(std::string& line) {
  line.push_back('\n');
  // A line that cannot be written (stdout closed or full) has nowhere else to go.
  static_cast<void>(std::fwrite(line.data(), 1, line.size(), stdout));
}

// Logs 'message' as is.
inline void WriteLine(level::level_enum lvl, std::string_view message) {
  std::string line = StartLine(lvl, message.size());
  line.append(message);
  EndLine(line);
}

// Logs 'fmt' formatted with 'args'. Not a template: the call sites share its code.
inline void WriteLine(level::level_enum lvl, std::string_view fmt, std::format_args args) {
  std::string line = StartLine(lvl, fmt.size());
  std::vformat_to(std::back_inserter(line), fmt, args);
  EndLine(line);
}

}  // namespace detail

template <typename... Args>
void log(level::level_enum level, std::string_view fmt, Args&&... args) {
  if (static_cast<int>(get_level()) <= static_cast<int>(level)) {
    if constexpr (sizeof...(Args) == 0) {
      detail::WriteLine(level, fmt);
    } else {
      detail::WriteLine(level, fmt, std::make_format_args(args...));
    }
  }
}

template <typename... Args>
void trace(std::string_view fmt, Args&&... args) {
  log(level::trace, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void debug(std::string_view fmt, Args&&... args) {
  log(level::debug, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void info(std::string_view fmt, Args&&... args) {
  log(level::info, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void warn(std::string_view fmt, Args&&... args) {
  log(level::warn, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void error(std::string_view fmt, Args&&... args) {
  log(level::err, fmt, std::forward<Args>(args)...);
}
template <typename... Args>
void critical(std::string_view fmt, Args&&... args) {
  log(level::critical, fmt, std::forward<Args>(args)...);
}

}  // namespace log
#endif

}  // namespace aeronet
