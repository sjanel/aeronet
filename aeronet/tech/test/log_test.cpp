#include "aeronet/log.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "aeronet/format-lite.hpp"
#include "aeronet/log-capture.hpp"

#ifdef AERONET_ENABLE_SPDLOG
#include <spdlog/common.h>
#include <spdlog/spdlog.h>
#endif

namespace aeronet {

namespace {

// Sets the level of the logger, restored on destruction.
class ScopedLogLevel {
 public:
  explicit ScopedLogLevel(log::level::level_enum lvl) : _previous(log::get_level()) { log::set_level(lvl); }

  ScopedLogLevel(const ScopedLogLevel&) = delete;
  ScopedLogLevel(ScopedLogLevel&&) = delete;
  ScopedLogLevel& operator=(const ScopedLogLevel&) = delete;
  ScopedLogLevel& operator=(ScopedLogLevel&&) = delete;

  ~ScopedLogLevel() { log::set_level(_previous); }

 private:
  log::level::level_enum _previous;
};

// The lines of 'text', without their new line.
std::vector<std::string_view> Lines(std::string_view text) {
  std::vector<std::string_view> lines;
  while (!text.empty()) {
    const std::size_t newLinePos = text.find('\n');
    lines.push_back(text.substr(0, newLinePos));
    if (newLinePos == std::string_view::npos) {
      break;
    }
    text.remove_prefix(newLinePos + 1);
  }
  return lines;
}

}  // namespace

TEST(LogTest, LevelFiltersTheMessages) {
  if (!test::LogCapture::kCapturesLogs) {
    GTEST_SKIP() << "the logs are not captured on this platform";
  }
  ScopedLogLevel level(log::level::warn);
  test::LogCapture capture;
  log::info("info message {}", 1);
  log::warn("warn message {}", 2);
  log::error("error message without arguments {}");  // printed as is
  log::set_level(log::level::off);
  log::critical("critical message {}", 3);

  const std::string logs = capture.take();
  EXPECT_FALSE(logs.contains("info message")) << logs;
  EXPECT_TRUE(logs.contains("warn message 2\n")) << logs;
  EXPECT_TRUE(logs.contains("error message without arguments {}\n")) << logs;
  EXPECT_FALSE(logs.contains("critical message")) << logs;
}

TEST(LogTest, EveryCallWritesAtOrAboveTheCurrentLevel) {
  if (!test::LogCapture::kCapturesLogs) {
    GTEST_SKIP() << "the logs are not captured on this platform";
  }
  ScopedLogLevel level(log::level::trace);
  EXPECT_EQ(log::get_level(), log::level::trace);
  test::LogCapture capture;
  log::trace("trace formatted {}", 0);
  log::trace("trace as is {}");
  log::debug("debug formatted {}", 1);
  log::debug("debug as is {}");
  log::info("info formatted {}", 2);
  log::info("info as is {}");
  log::warn("warn formatted {}", 3);
  log::warn("warn as is {}");
  log::error("error formatted {}", 4);
  log::error("error as is {}");
  log::critical("critical formatted {}", 5);
  log::critical("critical as is {}");
  log::log(log::level::info, "log formatted {} {}", 6, "text");
  log::log(log::level::info, "log as is {}");

  log::set_level(log::level::critical);
  EXPECT_EQ(log::get_level(), log::level::critical);
  log::trace("filtered {}", 0);
  log::trace("filtered");
  log::debug("filtered {}", 1);
  log::debug("filtered");
  log::info("filtered {}", 2);
  log::info("filtered");
  log::warn("filtered {}", 3);
  log::warn("filtered");
  log::error("filtered {}", 4);
  log::error("filtered");
  log::log(log::level::err, "filtered {}", 5);
  log::log(log::level::err, "filtered");
  log::critical("still written {}", 6);

  const std::string logs = capture.take();
  static constexpr std::array kExpected{
      "trace formatted 0\n",    "trace as is {}\n", "debug formatted 1\n",    "debug as is {}\n",
      "info formatted 2\n",     "info as is {}\n",  "warn formatted 3\n",     "warn as is {}\n",
      "error formatted 4\n",    "error as is {}\n", "critical formatted 5\n", "critical as is {}\n",
      "log formatted 6 text\n", "log as is {}\n",   "still written 6\n",
  };
  for (std::string_view expected : kExpected) {
    EXPECT_TRUE(logs.contains(expected)) << expected << " not in " << logs;
  }
  EXPECT_FALSE(logs.contains("filtered")) << logs;
}

TEST(LogTest, FormatSpecifications) {
  if (!test::LogCapture::kCapturesLogs) {
    GTEST_SKIP() << "the logs are not captured on this platform";
  }
  ScopedLogLevel level(log::level::info);
  test::LogCapture capture;
  log::info("{:#06x} [{:>5}] {:.2f} {{escaped}} {}", 255, "ab", 1.5, true);

  const std::string logs = capture.take();
  EXPECT_TRUE(logs.contains("0x00ff [   ab] 1.50 {escaped} true\n")) << logs;
}

// A message that cannot be formatted is dropped with a line on stderr: the call does not throw.
TEST(LogTest, MessageThatCannotBeFormattedIsDropped) {
  ScopedLogLevel level(log::level::info);
  std::string logs;
  testing::internal::CaptureStderr();
  {
    test::LogCapture capture;
    log::error("dropped {:c}", 1000000);  // not a char: std::format_error
    log::info("written after the failure {}", 1);
    logs = capture.take();
  }
  const std::string errors = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(errors.contains("aeronet::log dropped a message that could not be formatted or written")) << errors;
  if (test::LogCapture::kCapturesLogs) {
    EXPECT_FALSE(logs.contains("dropped")) << logs;
    EXPECT_TRUE(logs.contains("written after the failure 1\n")) << logs;
  }
}

#ifdef AERONET_ENABLE_SPDLOG

TEST(LogTest, SetLevelAlsoSetsTheLevelOfSpdlog) {
  ScopedLogLevel level(log::level::err);
  EXPECT_EQ(spdlog::get_level(), spdlog::level::err);
  log::set_level(log::level::debug);
  EXPECT_EQ(spdlog::get_level(), spdlog::level::debug);
}

#endif

// Each line is written at once: the lines logged by concurrent threads do not interleave.
TEST(LogTest, LinesOfConcurrentThreadsDoNotInterleave) {
  if (!test::LogCapture::kCapturesLogs) {
    GTEST_SKIP() << "the logs are not captured on this platform";
  }
  static constexpr int kNbThreads = 8;
  static constexpr int kNbLinesPerThread = 200;
  const std::string padding(200, 'x');  // long lines, more likely to be split by a non atomic write

  std::unordered_set<std::string> expected;
  for (int threadIdx = 0; threadIdx < kNbThreads; ++threadIdx) {
    for (int lineIdx = 0; lineIdx < kNbLinesPerThread; ++lineIdx) {
      expected.insert(Format("thread {} line {} {}", threadIdx, lineIdx, padding));
    }
  }

  ScopedLogLevel level(log::level::info);
  std::string logs;
  {
    test::LogCapture capture;
    {
      std::vector<std::jthread> threads;
      threads.reserve(kNbThreads);
      for (int threadIdx = 0; threadIdx < kNbThreads; ++threadIdx) {
        threads.emplace_back([threadIdx, &padding] {
          for (int lineIdx = 0; lineIdx < kNbLinesPerThread; ++lineIdx) {
            log::info("thread {} line {} {}", threadIdx, lineIdx, padding);
          }
        });
      }
    }
    logs = capture.take();
  }

  const std::vector<std::string_view> lines = Lines(logs);
  ASSERT_EQ(lines.size(), expected.size());
  for (std::string_view line : lines) {
    ASSERT_TRUE(line.starts_with('[')) << line;
    const std::size_t messagePos = line.find("] thread ");
    ASSERT_NE(messagePos, std::string_view::npos) << line;
    EXPECT_EQ(expected.erase(std::string(line.substr(messagePos + 2))), 1U) << line;
  }
  EXPECT_TRUE(expected.empty());
}

#ifndef AERONET_ENABLE_SPDLOG

// The line of the fallback logger: '[YYYY-MM-DDThh:mm:ss.sssZ] [level] message'.
TEST(LogTest, FallbackLineFormat) {
  ScopedLogLevel level(log::level::trace);
  test::LogCapture capture;
  log::trace("{} and {}", 42, "text");
  log::critical("no argument");

  const std::string logs = capture.take();
  const std::vector<std::string_view> lines = Lines(logs);
  ASSERT_EQ(lines.size(), 2U) << logs;
  for (std::string_view line : lines) {
    ASSERT_GT(line.size(), 26U) << line;
    EXPECT_EQ(line[0], '[') << line;
    EXPECT_EQ(line[11], 'T') << line;
    EXPECT_EQ(line[20], '.') << line;
    EXPECT_EQ(line[24], 'Z') << line;
  }
  EXPECT_EQ(lines[0].substr(25), "] [trace] 42 and text");
  EXPECT_EQ(lines[1].substr(25), "] [critical] no argument");
  EXPECT_TRUE(logs.ends_with('\n'));
}

#endif

}  // namespace aeronet
