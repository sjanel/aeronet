#include "aeronet/telemetry-config.hpp"

#include <gtest/gtest.h>

#include <iterator>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "aeronet/scoped-env-var.hpp"

namespace aeronet {

TEST(TelemetryConfigTest, DefaultShouldValidate) {
  TelemetryConfig cfg;
  EXPECT_NO_THROW(cfg.validate());
}

TEST(TelemetryConfigTest, ValidateAcceptsValidConfig) {
  TelemetryConfig cfg;
  cfg.withEndpoint("http://localhost:4318")
      .withServiceName("myservice")
      .withDogStatsdSocketPath("/var/run/datadog/dsd.socket")
      .withDogStatsdNamespace("myapp")
      .withSampleRate(0.5);
  EXPECT_NO_THROW(cfg.validate());
}

TEST(TelemetryConfigTest, ValidateRejectsInvalidSampleRate) {
  TelemetryConfig cfg;
  cfg.withSampleRate(-0.1);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  cfg.withSampleRate(1.1);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(TelemetryConfigTest, AddDogStatsD) {
  TelemetryConfig cfg;
  cfg.withDogStatsdSocketPath("/var/run/datadog/dsd.socket")
      .withServiceName("testservice")
      .addDogStatsdTag("env:testing")
      .addHttpHeader("Authorization", "ApiKey 12345")
      .addHttpHeader("Custom-Header", "CustomValue");
  cfg.validate();

  EXPECT_TRUE(cfg.dogstatsdTags().fullString().contains("env:testing"));

  const auto headers = cfg.httpHeadersRange();
  EXPECT_EQ(std::distance(headers.begin(), headers.end()), 2);
  auto it = headers.begin();
  EXPECT_EQ(*it, "Authorization:ApiKey 12345");
  ++it;
  EXPECT_EQ(*it, "Custom-Header:CustomValue");
}

TEST(TelemetryConfigTest, TelemetryConfigInvalidSampleRateThrows) {
  TelemetryConfig cfg;
  cfg.sampleRate = -0.1;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  TelemetryConfig cfgHigh;
  cfgHigh.sampleRate = 1.5;
  EXPECT_THROW(cfgHigh.validate(), std::invalid_argument);
}

TEST(TelemetryConfigTest, TelemetryConfigDogStatsDTakesEnvSocket) {
  // Ensure DD_DOGSTATSD_SOCKET_PATH is unset so the DD_DOGSTATSD_SOCKET fallback is exercised.
  test::ScopedEnvVar unsetPath("DD_DOGSTATSD_SOCKET_PATH", nullptr);
  test::ScopedEnvVar socketEnv("DD_DOGSTATSD_SOCKET", "/tmp/aeronet-dsd.sock");

  TelemetryConfig cfg;
  cfg.dogStatsDEnabled = true;
  cfg.validate();
  EXPECT_EQ(cfg.dogstatsdSocketPath(), "/tmp/aeronet-dsd.sock");
}

TEST(TelemetryConfigTest, TelemetryConfigDogStatsDEnabledWithoutPathThrows) {
  test::ScopedEnvVar unsetSocket("DD_DOGSTATSD_SOCKET", nullptr);
  test::ScopedEnvVar unsetPath("DD_DOGSTATSD_SOCKET_PATH", nullptr);

  TelemetryConfig cfg;
  cfg.dogStatsDEnabled = true;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(TelemetryConfigTest, DD_DOGSTATSD_SOCKETSet) {
  test::ScopedEnvVar socketEnv("DD_DOGSTATSD_SOCKET_PATH", "/tmp/aeronet-dsd.sock");

  TelemetryConfig cfg;
  cfg.dogStatsDEnabled = true;
  cfg.validate();
  EXPECT_EQ(cfg.dogstatsdSocketPath(), "/tmp/aeronet-dsd.sock");
}

TEST(TelemetryConfigTest, TelemetryConfigHttpHeadersStored) {
  TelemetryConfig cfg;
  cfg.addHttpHeader("Authorization", "Bearer secret-token");
  cfg.addHttpHeader("X-Test", "Value 42");

  std::vector<std::string_view> headers;
  auto range = cfg.httpHeadersRange();
  headers.assign(range.begin(), range.end());

  ASSERT_EQ(headers.size(), 2UL);
  EXPECT_EQ(headers[0], "Authorization:Bearer secret-token");
  EXPECT_EQ(headers[1], "X-Test:Value 42");
}

TEST(TelemetryConfigTest, TelemetryConfigServiceTagAppendedOnce) {
  TelemetryConfig cfg;
  cfg.withServiceName("svc-aeronet");

  cfg.validate();  // first call should append service tag
  cfg.validate();  // second call should not duplicate the tag

  std::vector<std::string_view> tags;
  auto range = cfg.dogstatsdTagsRange();
  tags.assign(range.begin(), range.end());

  ASSERT_EQ(tags.size(), 1UL);
  EXPECT_EQ(tags.front(), "service:svc-aeronet");
}

TEST(TelemetryConfigTest, InvalidHeader) {
  TelemetryConfig cfg;
  EXPECT_THROW(cfg.addHttpHeader("Invalid-Header-Name:", "Some value"), std::invalid_argument);
  EXPECT_THROW(cfg.addHttpHeader("Valid-Name", "Invalid\rValue"), std::invalid_argument);
  EXPECT_NO_THROW(cfg.addHttpHeader("Valid-Name", "Valid Value"));
}

}  // namespace aeronet