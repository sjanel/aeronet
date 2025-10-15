#include <gtest/gtest.h>

#include "aeronet/otel-config.hpp"
#include "aeronet/tracing/tracer.hpp"
#ifndef AERONET_ENABLE_OPENTELEMETRY
#include "invalid_argument_exception.hpp"
#endif

using namespace aeronet;

namespace {
constexpr bool kDefaultEnabled = aeronet::tracing::enabled();
}

// Test basic TelemetryContext functionality
TEST(OpenTelemetryIntegration, Lifecycle) {
  tracing::TelemetryContext telemetry;

  // Initialize with valid config
  OtelConfig cfg;
  cfg.enabled = kDefaultEnabled;
  cfg.endpoint = "http://localhost:4318/v1/traces";
  cfg.serviceName = "aeronet-integration-test";
  cfg.sampleRate = 1.0;

  telemetry = tracing::TelemetryContext(cfg);
}

TEST(OpenTelemetryIntegration, CountersOperations) {
  tracing::TelemetryContext telemetry;

  // Should be safe to call even without initialization
  telemetry.counterAdd("test.counter", 10U);
  telemetry.counterAdd("test.counter", 5U);

  // Initialize
  OtelConfig cfg;
  cfg.enabled = kDefaultEnabled;
  cfg.endpoint = "http://localhost:4318/v1/metrics";
  cfg.serviceName = "aeronet-test";

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
  OtelConfig cfg;
  cfg.enabled = kDefaultEnabled;
  cfg.endpoint = "http://localhost:4318/v1/traces";
  cfg.serviceName = "aeronet-test";
  cfg.sampleRate = 1.0;

  telemetry = tracing::TelemetryContext(cfg);

  auto span2 = telemetry.createSpan("test-span-2");

  if (cfg.enabled) {
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

  OtelConfig cfg1;
  cfg1.enabled = kDefaultEnabled;
  cfg1.endpoint = "http://localhost:4318/v1/traces";
  cfg1.serviceName = "service-1";

  OtelConfig cfg2;
  cfg2.enabled = kDefaultEnabled;
  cfg2.endpoint = "http://localhost:4319/v1/traces";  // Different port
  cfg2.serviceName = "service-2";

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

// Additional tests that require OpenTelemetry at compile-time
TEST(OpenTelemetryIntegration, Disabled) {
  tracing::TelemetryContext telemetry;

  OtelConfig cfg;
  cfg.enabled = false;  // Explicitly disabled

  telemetry = tracing::TelemetryContext(cfg);

  // Operations should be no-ops
  auto span = telemetry.createSpan("should-be-null");
  EXPECT_EQ(span, nullptr);
}

#ifndef AERONET_ENABLE_OPENTELEMETRY

TEST(OpenTelemetryIntegration, ShouldThrowIfDisabledAndAsked) {
  OtelConfig cfg;
  cfg.enabled = true;

  // Should always return false when OpenTelemetry is disabled at compile-time
  EXPECT_THROW(tracing::TelemetryContext{cfg}, ::aeronet::invalid_argument);
}

#endif  // AERONET_ENABLE_OPENTELEMETRY
