#include <gtest/gtest.h>
#include <opentelemetry/proto/collector/metrics/v1/metrics_service.pb.h>
#include <opentelemetry/proto/collector/trace/v1/trace_service.pb.h>
#include <opentelemetry/proto/metrics/v1/metrics.pb.h>

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/otlp_test_collector.hpp"
#include "aeronet/router.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

namespace {

bool SpansContainHttpRequest(const ::opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest& proto) {
  for (const auto& resourceSpan : proto.resource_spans()) {
    for (const auto& scopeSpan : resourceSpan.scope_spans()) {
      for (const auto& span : scopeSpan.spans()) {
        if (span.name() == "http.request") {
          return true;
        }
      }
    }
  }
  return false;
}

bool ResourceContainsService(const ::opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest& proto,
                             std::string_view serviceName) {
  for (const auto& resourceSpan : proto.resource_spans()) {
    for (const auto& attr : resourceSpan.resource().attributes()) {
      if (attr.key() == "service.name" && attr.value().string_value() == serviceName) {
        return true;
      }
    }
  }
  return false;
}

bool MetricsContainCounter(const ::opentelemetry::proto::collector::metrics::v1::ExportMetricsServiceRequest& proto,
                           std::string_view metricName) {
  using ::opentelemetry::proto::metrics::v1::NumberDataPoint;
  for (const auto& resourceMetric : proto.resource_metrics()) {
    for (const auto& scopeMetric : resourceMetric.scope_metrics()) {
      for (const auto& metric : scopeMetric.metrics()) {
        if (metric.name() != metricName || !metric.has_sum()) {
          continue;
        }
        for (const auto& point : metric.sum().data_points()) {
          switch (point.value_case()) {
            case NumberDataPoint::kAsInt:
              if (point.as_int() > 0) {
                return true;
              }
              break;
            case NumberDataPoint::kAsDouble:
              if (point.as_double() > 0.0) {
                return true;
              }
              break;
            default:
              break;
          }
        }
      }
    }
  }
  return false;
}

}  // namespace

TEST(OpenTelemetryEndToEnd, EmitsTracesAndMetrics) {
  test::OtlpTestCollector collector;

  TelemetryConfig telemetryCfg;
  telemetryCfg.otelEnabled = true;
  telemetryCfg.withEndpoint(collector.endpointForTraces());
  telemetryCfg.withServiceName("aeronet-e2e");
  telemetryCfg.withSampleRate(1.0);
  telemetryCfg.addHttpHeader("x-test-auth", "otel-secret");

  HttpServerConfig serverCfg;
  serverCfg.withTelemetryConfig(telemetryCfg);
  serverCfg.enableKeepAlive = false;

  test::TestServer server(serverCfg);
  server.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK).body("otel-ok"); });

  const auto response = test::simpleGet(server.port(), "/otel");
  ASSERT_FALSE(response.empty());
  EXPECT_TRUE(response.contains("otel-ok"));

  std::vector<test::CapturedOtlpRequest> received;
  received.push_back(collector.waitForRequest(1s));
  received.push_back(collector.waitForRequest(10s));

  const test::CapturedOtlpRequest* traceReq = nullptr;
  const test::CapturedOtlpRequest* metricsReq = nullptr;
  for (const auto& req : received) {
    if (req.path == "/v1/traces") {
      traceReq = &req;
    } else if (req.path == "/v1/metrics") {
      metricsReq = &req;
    }
  }

  ASSERT_NE(traceReq, nullptr) << "Trace export not captured";
  ASSERT_NE(metricsReq, nullptr) << "Metrics export not captured";

  EXPECT_EQ(traceReq->method, "POST");
  EXPECT_EQ(traceReq->headerValue("x-test-auth"), "otel-secret");

  ::opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest traceProto;
  ASSERT_TRUE(traceProto.ParseFromString(traceReq->body));
  EXPECT_TRUE(SpansContainHttpRequest(traceProto));
  EXPECT_TRUE(ResourceContainsService(traceProto, "aeronet-e2e"));

  ::opentelemetry::proto::collector::metrics::v1::ExportMetricsServiceRequest metricsProto;
  ASSERT_TRUE(metricsProto.ParseFromString(metricsReq->body));
  EXPECT_TRUE(MetricsContainCounter(metricsProto, "aeronet.connections.accepted"));

  EXPECT_TRUE(collector.drain().empty());  // No extra requests
}
