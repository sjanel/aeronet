#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef __unix__
#include <sys/socket.h>
#include <sys/un.h>
#endif

#include "aeronet/base-fd.hpp"
#include "aeronet/features.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/tracing/tracer.hpp"

using namespace aeronet;

namespace {
constexpr bool kDefaultEnabled = aeronet::openTelemetryEnabled();

class ScopedEnvVar {
 public:
  ScopedEnvVar(const char* name, const char* value) : _name(name) {
    if (const char* prev = std::getenv(name)) {
      _hadOld = true;
      _old = prev;
    }
#ifdef _WIN32
    if (value != nullptr) {
      _putenv_s(name, value);
    } else {
      _putenv_s(name, "");
    }
#else
    if (value != nullptr) {
      ::setenv(name, value, 1);  // NOLINT(misc-include-cleaner) cstdlib header
    } else {
      ::unsetenv(name);  // NOLINT(misc-include-cleaner) cstdlib header
    }
#endif
  }

  ~ScopedEnvVar() {
#ifdef _WIN32
    if (_hadOld) {
      _putenv_s(_name.c_str(), _old.c_str());
    } else {
      _putenv_s(_name.c_str(), "");
    }
#else
    if (_hadOld) {
      ::setenv(_name.c_str(), _old.c_str(), 1);  // NOLINT(misc-include-cleaner) cstdlib header
    } else {
      ::unsetenv(_name.c_str());  // NOLINT(misc-include-cleaner) cstdlib header
    }
#endif
  }

 private:
  std::string _name;
  std::string _old;
  bool _hadOld = false;
};
}  // namespace

// Test basic TelemetryContext functionality
TEST(OpenTelemetryIntegration, Lifecycle) {
  tracing::TelemetryContext telemetry;

  // Initialize with valid config
  TelemetryConfig cfg;
  cfg.otelEnabled = kDefaultEnabled;
  cfg.withEndpoint("http://localhost:4318/v1/traces");
  cfg.withServiceName("aeronet-integration-test");
  cfg.withSampleRate(1.0);

  telemetry = tracing::TelemetryContext(cfg);
}

TEST(OpenTelemetryIntegration, CountersOperations) {
  tracing::TelemetryContext telemetry;

  // Should be safe to call even without initialization
  telemetry.counterAdd("test.counter", 10U);
  telemetry.counterAdd("test.counter", 5U);

  // Initialize
  TelemetryConfig cfg;
  cfg.otelEnabled = kDefaultEnabled;
  cfg.withEndpoint("http://localhost:4318/v1/metrics");
  cfg.withServiceName("aeronet-test");

  telemetry = tracing::TelemetryContext(cfg);

  // Should work after initialization (or silently fail)
  telemetry.counterAdd("events.processed", 100U);
  telemetry.counterAdd("bytes.written", 1024U);
}

TEST(OpenTelemetryIntegration, SpanOperations) {
  tracing::TelemetryContext telemetry;

  // Should return nullptr before initialization
  auto span1 = telemetry.createSpan("test-span-1");
  EXPECT_EQ(span1, nullptr);

  // Initialize
  TelemetryConfig cfg;
  cfg.otelEnabled = kDefaultEnabled;
  cfg.withEndpoint("http://localhost:4318/v1/traces");
  cfg.withServiceName("aeronet-test");
  cfg.withSampleRate(1.0);

  telemetry = tracing::TelemetryContext(cfg);

  auto span2 = telemetry.createSpan("test-span-2");

  if (cfg.otelEnabled) {
    ASSERT_NE(span2, nullptr);

    span2->setAttribute("test.key", "test.value");
    span2->setAttribute("test.number", 42);
    span2->end();
  } else {
    EXPECT_EQ(span2, nullptr);
  }
}

TEST(OpenTelemetryIntegration, IndependentContexts) {
  // Test that multiple TelemetryContext instances are independent
  tracing::TelemetryContext telemetry1;
  tracing::TelemetryContext telemetry2;

  TelemetryConfig cfg1;
  cfg1.otelEnabled = kDefaultEnabled;
  cfg1.withEndpoint("http://localhost:4318/v1/traces");
  cfg1.withServiceName("service-1");

  TelemetryConfig cfg2;
  cfg2.otelEnabled = kDefaultEnabled;
  cfg2.withEndpoint("http://localhost:4319/v1/traces");  // Different port
  cfg2.withServiceName("service-2");

  telemetry1 = tracing::TelemetryContext(cfg1);
  telemetry2 = tracing::TelemetryContext(cfg2);

  // Operations on one context don't affect the other
  telemetry1.counterAdd("context1.counter");
  telemetry2.counterAdd("context2.counter");

  auto span1 = telemetry1.createSpan("context1-span");
  auto span2 = telemetry2.createSpan("context2-span");

  // Clean up
  if (span1) {
    span1->end();
  }
  if (span2) {
    span2->end();
  }
}

TEST(OpenTelemetryIntegration, TelemetryConfigHttpHeadersStored) {
  TelemetryConfig cfg;
  cfg.addHttpHeader("Authorization", "Bearer secret-token");
  cfg.addHttpHeader("X-Test", "Value 42");

  std::vector<std::string_view> headers;
  auto range = cfg.httpHeadersRange();
  headers.assign(range.begin(), range.end());

  ASSERT_EQ(headers.size(), 2UL);
  EXPECT_EQ(headers[0], "Authorization: Bearer secret-token");
  EXPECT_EQ(headers[1], "X-Test: Value 42");
}

TEST(OpenTelemetryIntegration, TelemetryConfigServiceTagAppendedOnce) {
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

TEST(OpenTelemetryIntegration, TelemetryConfigInvalidSampleRateThrows) {
  TelemetryConfig cfg;
  cfg.sampleRate = -0.1;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  TelemetryConfig cfgHigh;
  cfgHigh.sampleRate = 1.5;
  EXPECT_THROW(cfgHigh.validate(), std::invalid_argument);
}

TEST(OpenTelemetryIntegration, TelemetryConfigDogStatsDTakesEnvSocket) {
  // Ensure DD_DOGSTATSD_SOCKET_PATH is unset so the DD_DOGSTATSD_SOCKET fallback is exercised.
  ScopedEnvVar unsetPath("DD_DOGSTATSD_SOCKET_PATH", nullptr);
  ScopedEnvVar socketEnv("DD_DOGSTATSD_SOCKET", "/tmp/aeronet-dsd.sock");

  TelemetryConfig cfg;
  cfg.dogStatsDEnabled = true;
  cfg.validate();
  EXPECT_EQ(cfg.dogstatsdSocketPath(), "/tmp/aeronet-dsd.sock");
}

TEST(OpenTelemetryIntegration, TelemetryConfigDogStatsDEnabledWithoutPathThrows) {
  ScopedEnvVar unsetSocket("DD_DOGSTATSD_SOCKET", nullptr);
  ScopedEnvVar unsetPath("DD_DOGSTATSD_SOCKET_PATH", nullptr);

  TelemetryConfig cfg;
  cfg.dogStatsDEnabled = true;
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

// Additional tests that require OpenTelemetry at compile-time
TEST(OpenTelemetryIntegration, Disabled) {
  tracing::TelemetryContext telemetry;

  TelemetryConfig cfg;
  cfg.otelEnabled = false;  // Explicitly disabled

  telemetry = tracing::TelemetryContext(cfg);

  // Operations should be no-ops
  auto span = telemetry.createSpan("should-be-null");
  EXPECT_EQ(span, nullptr);
}

#ifdef __unix__
TEST(OpenTelemetryIntegration, DogStatsDMetricsEmission) {
  // Create an isolated temporary directory and use a socket path inside it.
  aeronet::test::ScopedTempDir tmpDir("aeronet-dsd-dir-");
  const auto socketPath = tmpDir.dirPath() / "aeronet-dsd.sock";

  aeronet::BaseFd serverFd(::socket(AF_UNIX, SOCK_DGRAM, 0));
  ASSERT_TRUE(serverFd);

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socketPath.c_str());
  ASSERT_EQ(::bind(serverFd.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

  // Use test helper to set a receive timeout on the socket.
  aeronet::test::setRecvTimeout(serverFd.fd(), std::chrono::seconds{1});

  TelemetryConfig cfg;
  cfg.otelEnabled = false;
  cfg.dogStatsDEnabled = true;
  cfg.withDogStatsdSocketPath(socketPath.string());
  cfg.withDogStatsdNamespace("aeronet");
  cfg.withServiceName("test-service");
  // Ensure default tags (service:) are appended to the dogstatsd tags
  cfg.validate();

  tracing::TelemetryContext telemetry(cfg);
  telemetry.counterAdd("test.metric", 7);

  // Use test util's recvWithTimeout which handles non-blocking reads and timeouts.
  auto payload = aeronet::test::recvWithTimeout(serverFd.fd(), std::chrono::seconds{1});
  ASSERT_FALSE(payload.empty());
  EXPECT_TRUE(payload.contains("aeronet.test.metric:7|c"));
  EXPECT_TRUE(payload.contains("service:test-service"));
}
#endif

#ifndef AERONET_ENABLE_OPENTELEMETRY

TEST(OpenTelemetryIntegration, ShouldThrowIfDisabledAndAsked) {
  TelemetryConfig cfg;
  cfg.otelEnabled = true;

  // Should always return false when OpenTelemetry is disabled at compile-time
  EXPECT_THROW(tracing::TelemetryContext{cfg}, std::invalid_argument);
}

#endif  // AERONET_ENABLE_OPENTELEMETRY
