#pragma once

#ifdef AERONET_ENABLE_SPDLOG
#include <spdlog/common.h>  // IWYU pragma: export
#include <spdlog/spdlog.h>  // IWYU pragma: export
#else
#include <format>
#include <iostream>
#endif

namespace aeronet {
#ifdef AERONET_ENABLE_SPDLOG
namespace log = spdlog;
#else
namespace log {

template <class... Args>
void critical(Args &&...args) {
  std::cout << "[critical] " << std::format(std::forward<Args>(args)...) << '\n';
}

template <class... Args>
void error(Args &&...args) {
  std::cout << "[error] " << std::format(std::forward<Args>(args)...) << '\n';
}

template <class... Args>
void warn(Args &&...args) {
  std::cout << "[warn] " << std::format(std::forward<Args>(args)...) << '\n';
}

template <class... Args>
void info(Args &&...args) {
  std::cout << "[info] " << std::format(std::forward<Args>(args)...) << '\n';
}

template <class... Args>
void debug(Args &&...args) {
  std::cout << "[debug] " << std::format(std::forward<Args>(args)...) << '\n';
}

template <class... Args>
void trace(Args &&...args) {
  std::cout << "[trace] " << std::format(std::forward<Args>(args)...) << '\n';
}

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
#endif

}  // namespace aeronet
