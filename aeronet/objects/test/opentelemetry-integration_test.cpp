#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/base-fd.hpp"
#include "aeronet/features.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/tracing/tracer.hpp"

using namespace aeronet;

namespace {
constexpr bool kDefaultEnabled = aeronet::openTelemetryEnabled();
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

TEST(OpenTelemetryIntegration, Endpoint) {
  tracing::TelemetryContext telemetry;

  TelemetryConfig cfg;
  cfg.otelEnabled = kDefaultEnabled;
  cfg.withEndpoint("http://localhost:4318");

  telemetry = tracing::TelemetryContext(cfg);

  cfg.withEndpoint("http://localhost:4318/");
  telemetry = tracing::TelemetryContext(cfg);
}

TEST(OpenTelemetryIntegration, HistogramBuckets) {
  tracing::TelemetryContext telemetry;

  TelemetryConfig cfg;
  cfg.otelEnabled = kDefaultEnabled;
  cfg.withEndpoint("http://localhost:4318/v1/metrics");
  cfg.withServiceName("aeronet-integration-test");

  // Configure custom histogram buckets
  TelemetryConfig::HistogramBoundariesMap buckets;
  cfg.addHistogramBuckets("test.histogram", std::vector{0.1, 1.0, 10.0, 100.0});
  cfg.addHistogramBuckets("test.histogram2", std::vector{1.0, 2.0});

  telemetry = tracing::TelemetryContext(cfg);
}

TEST(OpenTelemetryIntegration, MetricsOperations) {
  tracing::TelemetryContext telemetry;

  // Should be safe to call even without initialization
  auto span = telemetry.createSpan("test.span");
  EXPECT_EQ(span, nullptr);
  telemetry.counterAdd("test.counter", 10U);
  telemetry.counterAdd("test.counter", 5U);
  telemetry.gauge("test.gauge", 3);
  telemetry.histogram("test.histogram", 3.14);
  telemetry.timing("test.timing", std::chrono::milliseconds(100));

  // Initialize
  TelemetryConfig cfg;
  cfg.withEndpoint("http://localhost:4318/v1/metrics");
  cfg.withServiceName("aeronet-test");

  for (bool otelEnabled : {false, kDefaultEnabled}) {
    for (bool dogStatsDEnabled : {false, true}) {
      cfg.otelEnabled = otelEnabled;
      cfg.dogStatsDEnabled = dogStatsDEnabled;
      telemetry = tracing::TelemetryContext(cfg);

      // Should work after initialization (or silently fail)
      span = telemetry.createSpan("test.span");
      EXPECT_EQ(span != nullptr, otelEnabled);
      telemetry.counterAdd("events.processed", 100U);
      telemetry.counterAdd("bytes.written", 1024U);
      telemetry.gauge("test.gauge", 3);
      telemetry.gauge("test.gauge2", 3);
      telemetry.histogram("test.histogram", 2.71);
      telemetry.histogram("test.histogram2", 1.61);
      telemetry.timing("test.timing", std::chrono::milliseconds(250));
      telemetry.timing("test.timing2", std::chrono::milliseconds(500));
      cfg.withEndpoint("http://localhost:4318/v1/metrics/");
    }
  }
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

  cfg.otelEnabled = false;
  cfg.dogStatsDEnabled = true;
  telemetry = tracing::TelemetryContext(cfg);

  auto span3 = telemetry.createSpan("test-span-3");
  EXPECT_EQ(span3, nullptr);
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

  telemetry1.gauge("context1.gauge", 1);
  telemetry2.gauge("context2.gauge", 2);

  telemetry1.histogram("context1.histogram", 1.1);
  telemetry2.histogram("context2.histogram", 2.2);

  telemetry1.timing("context1.timing", std::chrono::milliseconds(150));
  telemetry2.timing("context2.timing", std::chrono::milliseconds(250));

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

TEST(OpenTelemetryIntegration, DogStatsDMetricsEmission) {
  // Create an isolated temporary directory and use a socket path inside it.
  test::ScopedTempDir tmpDir("aeronet-dsd-dir-");
  const auto socketPath = tmpDir.dirPath() / "aeronet-dsd.sock";

  BaseFd serverFd(::socket(AF_UNIX, SOCK_DGRAM, 0));
  ASSERT_TRUE(serverFd);

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socketPath.c_str());
  ASSERT_EQ(::bind(serverFd.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);

  // Use test helper to set a receive timeout on the socket.
  test::setRecvTimeout(serverFd.fd(), std::chrono::seconds{1});

  TelemetryConfig cfg;
  cfg.otelEnabled = kDefaultEnabled;
  cfg.dogStatsDEnabled = true;
  cfg.withDogStatsdSocketPath(socketPath.string());
  cfg.withDogStatsdNamespace("aeronet");
  cfg.withServiceName("test-service");
  // Ensure default tags (service:) are appended to the dogstatsd tags
  cfg.validate();

  tracing::TelemetryContext telemetry(cfg);
  telemetry.counterAdd("test.metric", 7);
  telemetry.gauge("test.gauge", 3);
  telemetry.histogram("test.histogram", 4.25);
  telemetry.timing("test.timing", std::chrono::milliseconds(15));

  // Use test util's recvWithTimeout which handles non-blocking reads and timeouts.
  auto payload = test::recvWithTimeout(serverFd.fd(), std::chrono::milliseconds{100});
  EXPECT_TRUE(payload.contains("aeronet.test.metric:7|c"));
  EXPECT_EQ(payload.contains("service:test-service"), kDefaultEnabled);
  EXPECT_TRUE(payload.contains("aeronet.test.histogram:4.25|h"));
  EXPECT_TRUE(payload.contains("aeronet.test.timing:15|ms"));
}

TEST(OpenTelemetryIntegration, DogStatsDClientRetrieveNull) {
  tracing::TelemetryContext telemetry;
  EXPECT_EQ(telemetry.dogstatsdClient(), nullptr);
}

#ifdef AERONET_ENABLE_OPENTELEMETRY

TEST(OpenTelemetryIntegration, NoServiceName) {
  TelemetryConfig cfg;
  cfg.otelEnabled = kDefaultEnabled;
  cfg.withEndpoint("http://localhost:4318/v1/traces");
  // Intentionally omit service name

  // should be using default resource attributes
  tracing::TelemetryContext telemetry(cfg);
  EXPECT_NE(telemetry.createSpan("span-without-service-name"), nullptr);
}

TEST(OpenTelemetryIntegration, EmptyEndpoint) {
  TelemetryConfig cfg;
  cfg.otelEnabled = kDefaultEnabled;
  cfg.withEndpoint("");  // Empty endpoint
  cfg.withServiceName("test-service");

  // should be using default resource attributes
  tracing::TelemetryContext telemetry(cfg);
  EXPECT_NE(telemetry.createSpan("span-without-service-name"), nullptr);
}

#if AERONET_WANT_MALLOC_OVERRIDES

TEST(OpenTelemetryIntegration, MallocFailureHandling) {
  TelemetryConfig cfg;
  cfg.otelEnabled = kDefaultEnabled;
  cfg.withEndpoint("http://localhost:4318/v1/traces");
  cfg.withServiceName("aeronet-test");

  tracing::TelemetryContext telemetry(cfg);

  // Set to fail next malloc
  test::FailNextMalloc(1);  // no successes, 1 failure

  // If malloc failure was injected, telemetry should be a no-op instance
  EXPECT_NO_THROW(telemetry.counterAdd("test.should-fail", 1));
  test::FailNextMalloc(1);
  EXPECT_NO_THROW(telemetry.gauge("test.should-fail-gauge", 1));
  test::FailNextMalloc(1);
  EXPECT_NO_THROW(telemetry.histogram("test.should-fail-histogram", 1.0));
  test::FailNextMalloc(1);
  EXPECT_NO_THROW(telemetry.timing("test.should-fail-timing", std::chrono::milliseconds(100)));
}

#endif

#else

TEST(OpenTelemetryIntegration, ShouldThrowIfDisabledAndAsked) {
  TelemetryConfig cfg;
  cfg.otelEnabled = true;

  // Should always return false when OpenTelemetry is disabled at compile-time
  EXPECT_THROW(tracing::TelemetryContext{cfg}, std::invalid_argument);
}

#endif  // AERONET_ENABLE_OPENTELEMETRY
