#include "aeronet/access-log-writer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#ifdef AERONET_POSIX
#define AERONET_WANT_READ_WRITE_OVERRIDES
#include "aeronet/sys-test-support.hpp"
#endif

#include "aeronet/access-log-config.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-version.hpp"
#include "aeronet/request-metrics.hpp"

namespace aeronet {
namespace {

RequestMetrics MakeSampleMetrics() {
  RequestMetrics metrics;
  metrics.status = 200;
  metrics.method = http::Method::GET;
  metrics.version = http::Version(1, 1);
  metrics.reusedConnection = false;
  metrics.path = "/api/test";
  metrics.clientIp = "192.168.1.42";
  metrics.userAgent = "TestAgent/1.0";
  metrics.bytesIn = 128;
  metrics.bytesOut = 4096;
  metrics.duration = std::chrono::microseconds(1500);
  return metrics;
}

class AccessLogWriterTest : public ::testing::Test {
 protected:
  void SetUp() override { _tmpFile = std::filesystem::temp_directory_path() / "access-log-test.log"; }

  void TearDown() override { std::filesystem::remove(_tmpFile); }

  std::string readLogFile() {
    std::ifstream ifs(_tmpFile);
    std::stringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
  }

  std::filesystem::path _tmpFile;
};

TEST_F(AccessLogWriterTest, SinkNoneProducesNoOutput) {
  AccessLogConfig config;
  config.sink = AccessLogConfig::Sink::None;
  config.format = AccessLogConfig::Format::CLF;

  AccessLogWriter writer(config);
  EXPECT_FALSE(writer);
  writer.flush();
  // No crash, no file created
  EXPECT_FALSE(std::filesystem::exists(_tmpFile));
}

TEST_F(AccessLogWriterTest, CLFFormatToFile) {
  AccessLogConfig config;
  config.sink = AccessLogConfig::Sink::File;
  config.format = AccessLogConfig::Format::CLF;
  auto tmpFileStr = _tmpFile.string();
  config.filePath.assign(tmpFileStr.data(), tmpFileStr.size());

  {
    AccessLogWriter writer(config);
    writer.log(MakeSampleMetrics());
    writer.flush();
  }

  auto content = readLogFile();
  ASSERT_FALSE(content.empty());

  // CLF line should contain these elements
  EXPECT_TRUE(content.contains("192.168.1.42")) << content;
  EXPECT_TRUE(content.contains("GET")) << content;
  EXPECT_TRUE(content.contains("/api/test")) << content;
  EXPECT_TRUE(content.contains("HTTP/1.1")) << content;
  EXPECT_TRUE(content.contains("200")) << content;
  EXPECT_TRUE(content.contains("4096")) << content;
  EXPECT_TRUE(content.contains("TestAgent/1.0")) << content;
  EXPECT_TRUE(content.contains(" - - [")) << content;
  // Ends with newline
  EXPECT_EQ(content.back(), '\n');
}

TEST_F(AccessLogWriterTest, JSONFormatToFile) {
  AccessLogConfig config;
  config.sink = AccessLogConfig::Sink::File;
  config.format = AccessLogConfig::Format::JSON;
  auto tmpFileStr = _tmpFile.string();
  config.filePath.assign(tmpFileStr.data(), tmpFileStr.size());

  {
    AccessLogWriter writer(config);
    writer.log(MakeSampleMetrics());
    writer.flush();
  }

  auto content = readLogFile();
  ASSERT_FALSE(content.empty());

  // JSON line should contain key-value pairs
  EXPECT_TRUE(content.contains(R"("method":"GET")")) << content;
  EXPECT_TRUE(content.contains(R"("path":"/api/test")")) << content;
  EXPECT_TRUE(content.contains(R"("status":200)")) << content;
  EXPECT_TRUE(content.contains(R"("bytesOut":4096)")) << content;
  EXPECT_TRUE(content.contains(R"("ip":"192.168.1.42")")) << content;
  EXPECT_TRUE(content.contains(R"("ua":"TestAgent/1.0")")) << content;
  EXPECT_TRUE(content.contains(R"("durationUs":)")) << content;
  EXPECT_TRUE(content.contains(R"("ts":)")) << content;
  // Ends with newline
  EXPECT_EQ(content.back(), '\n');
}

TEST_F(AccessLogWriterTest, MultipleLogEntriesAppend) {
  AccessLogConfig config;
  config.sink = AccessLogConfig::Sink::File;
  config.format = AccessLogConfig::Format::CLF;
  config.filePath = _tmpFile.string();

  {
    AccessLogWriter writer(config);
    auto metrics = MakeSampleMetrics();
    writer.log(metrics);
    metrics.path = "/second";
    metrics.status = 404;
    writer.log(metrics);
    writer.flush();
  }

  auto content = readLogFile();
  EXPECT_TRUE(content.contains("/api/test")) << content;
  EXPECT_TRUE(content.contains("/second")) << content;
  EXPECT_TRUE(content.contains("404")) << content;
}

TEST_F(AccessLogWriterTest, DestructorFlushes) {
  AccessLogConfig config;
  config.sink = AccessLogConfig::Sink::File;
  config.format = AccessLogConfig::Format::CLF;
  config.filePath = _tmpFile.string();

  {
    AccessLogWriter writer(config);
    writer.log(MakeSampleMetrics());
    // No explicit flush - destructor should flush
  }

  auto content = readLogFile();
  EXPECT_FALSE(content.empty());
  EXPECT_TRUE(content.contains("/api/test")) << content;
}

TEST_F(AccessLogWriterTest, EmptyPathAndUserAgent) {
  AccessLogConfig config;
  config.sink = AccessLogConfig::Sink::File;
  config.format = AccessLogConfig::Format::CLF;
  config.filePath = _tmpFile.string();

  {
    AccessLogWriter writer(config);
    RequestMetrics metrics = MakeSampleMetrics();
    metrics.path = "/";
    metrics.userAgent = "";
    writer.log(metrics);
    writer.flush();
  }

  auto content = readLogFile();
  ASSERT_FALSE(content.empty());
  EXPECT_TRUE(content.contains("GET / HTTP/1.1")) << content;
}

TEST_F(AccessLogWriterTest, FileSinkThrowsWhenOpenFails) {
  AccessLogConfig config;
  config.sink = AccessLogConfig::Sink::File;
  config.format = AccessLogConfig::Format::CLF;

  // Use a directory path to force file-open failure while keeping filePath non-empty.
  config.filePath = std::filesystem::temp_directory_path().string();

  EXPECT_THROW(static_cast<void>(AccessLogWriter(config)), std::runtime_error);
}

TEST_F(AccessLogWriterTest, StdoutSinkConstructionWithoutFilePath) {
  AccessLogConfig config;
  config.sink = AccessLogConfig::Sink::Stdout;
  config.format = AccessLogConfig::Format::CLF;

  AccessLogWriter writer(config);
  EXPECT_TRUE(writer);
}

TEST_F(AccessLogWriterTest, LogAutoFlushesWhenThresholdReached) {
  AccessLogConfig config;
  config.sink = AccessLogConfig::Sink::File;
  config.format = AccessLogConfig::Format::CLF;
  config.filePath = _tmpFile.string();

  RequestMetrics metrics = MakeSampleMetrics();
  std::string largeUa(9000, 'x');
  metrics.userAgent = largeUa;

  AccessLogWriter writer(config);
  writer.log(metrics);

  // No explicit flush: log() should auto-flush because payload exceeds threshold.
  auto content = readLogFile();
  EXPECT_FALSE(content.empty());
}

#ifdef AERONET_POSIX
TEST_F(AccessLogWriterTest, FlushStdoutHandlesPartialWrites) {
  AccessLogConfig config;
  config.sink = AccessLogConfig::Sink::Stdout;
  config.format = AccessLogConfig::Format::CLF;

  AccessLogWriter writer(config);
  RequestMetrics metrics = MakeSampleMetrics();
  std::string mediumUa(200, 'y');
  metrics.userAgent = mediumUa;
  writer.log(metrics);

  // Force write(1, ...) to return a partial result first, then complete.
  test::PushWriteAction(1, {10, 0});
  test::PushWriteAction(1, {8000, 0});
  writer.flush();

  test::ResetIoActions();
}

TEST_F(AccessLogWriterTest, FlushWriteErrorDisablesWriter) {
  AccessLogConfig config;
  config.sink = AccessLogConfig::Sink::Stdout;
  config.format = AccessLogConfig::Format::CLF;

  AccessLogWriter writer(config);
  writer.log(MakeSampleMetrics());

  // Simulate write failure for stdout fd.
  test::PushWriteAction(1, {-1, EIO});
  writer.flush();
  EXPECT_FALSE(writer);

  test::ResetIoActions();
}
#endif

}  // namespace
}  // namespace aeronet
