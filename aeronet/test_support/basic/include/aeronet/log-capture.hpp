#pragma once

#include <filesystem>
#include <string>

#include "aeronet/temp-file.hpp"

namespace aeronet::test {

// Captures what aeronet logs while alive, by redirecting the file descriptor of stdout - where the fallback logger and
// the default logger of spdlog write - to a temporary file. The other threads may keep logging meanwhile. The output
// not taken (gtest messages printed after the last take(), for instance) is written to the real stdout on destruction.
class LogCapture {
 public:
#if defined(_WIN32) && defined(AERONET_ENABLE_SPDLOG)
  // spdlog on Windows writes to the console handle it got when it started: its logs are not captured.
  static constexpr bool kCapturesLogs = false;
#else
  static constexpr bool kCapturesLogs = true;
#endif

  LogCapture();

  LogCapture(const LogCapture&) = delete;
  LogCapture(LogCapture&&) = delete;
  LogCapture& operator=(const LogCapture&) = delete;
  LogCapture& operator=(LogCapture&&) = delete;

  ~LogCapture();

  // Takes what was written to stdout since the construction or the previous take().
  [[nodiscard]] std::string take();

 private:
  ScopedTempDir _dir{"aeronet-log-capture-"};
  std::filesystem::path _path{_dir.dirPath() / "stdout"};
  int _savedStdout{-1};
};

}  // namespace aeronet::test
