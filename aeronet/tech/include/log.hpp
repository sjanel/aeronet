#pragma once

#include <cstdint>

#ifdef AERONET_DISABLE_SPDLOG
#include <string_view>
#else
#include <spdlog/common.h>  // IWYU pragma: export
#include <spdlog/spdlog.h>  // IWYU pragma: export
#endif

namespace aeronet {
#ifdef AERONET_DISABLE_SPDLOG
namespace log {
constexpr void critical(std::string_view, ...) {}
constexpr void error(std::string_view, ...) {}
constexpr void warn(std::string_view, ...) {}
constexpr void info(std::string_view, ...) {}
constexpr void debug(std::string_view, ...) {}
constexpr void trace(std::string_view, ...) {}

struct level {
  using level_enum = int;
  static constexpr int trace = 0;
  static constexpr int debug = 1;
  static constexpr int info = 2;
  static constexpr int warn = 3;
  static constexpr int error = 4;
  static constexpr int critical = 5;
  static constexpr int off = 6;
};

constexpr int get_level() { return static_cast<int>(level::off); }

}  // namespace log
#else
namespace log = spdlog;

#endif

enum class LogLevel : int8_t {
#ifdef AERONET_DISABLE_SPDLOG
  trace,
  debug,
  info,
  warn,
  err,
  critical,
  off
#else
  trace = static_cast<int8_t>(log::level::level_enum::trace),
  debug = static_cast<int8_t>(log::level::level_enum::debug),
  info = static_cast<int8_t>(log::level::level_enum::info),
  warn = static_cast<int8_t>(log::level::level_enum::warn),
  err = static_cast<int8_t>(log::level::level_enum::err),
  critical = static_cast<int8_t>(log::level::level_enum::critical),
  off = static_cast<int8_t>(log::level::level_enum::off)
#endif
};

constexpr int8_t PosFromLevel(LogLevel level) {
  return static_cast<int8_t>(LogLevel::off) - static_cast<int8_t>(level);
}

}  // namespace aeronet
