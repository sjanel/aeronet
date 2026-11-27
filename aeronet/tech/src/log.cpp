#include "aeronet/log.hpp"

#include <atomic>
#include <cstdio>
#include <format>
#include <iterator>
#include <string>
#include <string_view>

#ifdef AERONET_ENABLE_SPDLOG
// spdlog is compiled into this source only, header-only: the aeronet headers do not include it, and the aeronet
// libraries link no spdlog library.
#ifndef SPDLOG_HEADER_ONLY
#define SPDLOG_HEADER_ONLY
#endif
#include <spdlog/common.h>
#include <spdlog/logger.h>
#include <spdlog/spdlog.h>
#else
#include <cstddef>

#include "aeronet/time-constants.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/timestring.hpp"
#endif

namespace aeronet::log {

namespace detail {

constinit std::atomic<level::level_enum> g_level{level::info};

}  // namespace detail

namespace {

#ifdef AERONET_ENABLE_SPDLOG

static_assert(static_cast<int>(level::trace) == spdlog::level::trace &&
              static_cast<int>(level::debug) == spdlog::level::debug &&
              static_cast<int>(level::info) == spdlog::level::info &&
              static_cast<int>(level::warn) == spdlog::level::warn &&
              static_cast<int>(level::err) == spdlog::level::err &&
              static_cast<int>(level::critical) == spdlog::level::critical &&
              static_cast<int>(level::off) == spdlog::level::off);

constexpr spdlog::level::level_enum ToSpdlog(level::level_enum lvl) {
  return static_cast<spdlog::level::level_enum>(lvl);
}

void WriteLine(level::level_enum lvl, std::string_view message) {
  spdlog::default_logger_raw()->log(ToSpdlog(lvl), spdlog::string_view_t(message.data(), message.size()));
}

#else

constexpr std::string_view kLevelNames[]{
    "trace", "debug", "info", "warn", "error", "critical", "off",
};

// A line '[time] [level] ', with room for the message of 'messageSize' bytes and the final new line.
std::string StartLine(level::level_enum lvl, std::size_t messageSize) {
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
void EndLine(std::string& line) {
  line.push_back('\n');
  // A line that cannot be written (stdout closed or full) has nowhere else to go.
  static_cast<void>(std::fwrite(line.data(), 1, line.size(), stdout));
}

#endif

// Writes 'text' formatted with '*pArgs', or as is without arguments (null 'pArgs').
void Emit(level::level_enum lvl, std::string_view text, const std::format_args* pArgs) noexcept {
  try {
#ifdef AERONET_ENABLE_SPDLOG
    if (pArgs == nullptr) {
      WriteLine(lvl, text);
      return;
    }
    std::string message;
    std::vformat_to(std::back_inserter(message), text, *pArgs);
    WriteLine(lvl, message);
#else
    std::string line = StartLine(lvl, text.size());
    if (pArgs == nullptr) {
      line.append(text);
    } else {
      std::vformat_to(std::back_inserter(line), text, *pArgs);
    }
    EndLine(line);
#endif
  } catch (...) {
    // A formatter that throws, or a failed allocation: the logging call sites (destructors included) must not fail.
    std::fputs("aeronet::log dropped a message that could not be formatted or written\n", stderr);
  }
}

}  // namespace

void set_level(level::level_enum lvl) {
  detail::g_level.store(lvl, std::memory_order_relaxed);
#ifdef AERONET_ENABLE_SPDLOG
  spdlog::set_level(ToSpdlog(lvl));
#endif
}

namespace detail {

void Write(level::level_enum lvl, std::string_view message) noexcept { Emit(lvl, message, nullptr); }

void Write(level::level_enum lvl, std::string_view fmt, std::format_args args) noexcept { Emit(lvl, fmt, &args); }

}  // namespace detail

}  // namespace aeronet::log
