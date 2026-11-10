#include "aeronet/log.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <format>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

#include "aeronet/log-capture.hpp"

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
      expected.insert(std::format("thread {} line {} {}", threadIdx, lineIdx, padding));
    }
  }

  ScopedLogLevel level(log::level::info);
  std::string logs;
  {
    test::LogCapture capture;
    {
      std::vector<std::jthread> threads;
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
