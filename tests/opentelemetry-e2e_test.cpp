#include <gtest/gtest.h>
#include <opentelemetry/proto/collector/metrics/v1/metrics_service.pb.h>
#include <opentelemetry/proto/collector/trace/v1/trace_service.pb.h>
#include <opentelemetry/proto/metrics/v1/metrics.pb.h>

#ifdef AERONET_POSIX
#include <netinet/in.h>
#include <sys/socket.h>
#elifdef AERONET_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include "aeronet/errno-throw.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/log.hpp"
#include "aeronet/metric-label.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/otlp_test_collector.hpp"
#include "aeronet/socket.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/vector.hpp"

using namespace std::chrono_literals;

namespace aeronet {

namespace {

// A collector that never answers: its listening socket never accepts, so the connections of the exporters stay in its
// backlog and an export only ends at the exporter timeout. Closing it resets these connections.
class UnresponsiveCollector {
 public:
  UnresponsiveCollector() : _listen(Socket::Type::Stream) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // ephemeral
    if (::bind(_listen.fd(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      ThrowSystemError("bind unresponsive collector socket");
    }
    if (::listen(_listen.fd(), 16) != 0) {
      ThrowSystemError("listen unresponsive collector socket");
    }
    socklen_t len = sizeof(addr);  // NOLINT(misc-include-cleaner)
    if (::getsockname(_listen.fd(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
      ThrowSystemError("getsockname for unresponsive collector");
    }
    _port = ntohs(addr.sin_port);
  }

  [[nodiscard]] std::string endpointForTraces() const {
    return "http://127.0.0.1:" + std::to_string(_port) + "/v1/traces";
  }

  void close() noexcept { _listen.close(); }

 private:
  Socket _listen;
  uint16_t _port{0};
};

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

bool MetricsContainLabel(const ::opentelemetry::proto::collector::metrics::v1::ExportMetricsServiceRequest& proto,
                         std::string_view metricName, std::string_view key, std::string_view value) {
  const auto pointsContainLabel = [key, value](const auto& points) {
    for (const auto& point : points) {
      for (const auto& attribute : point.attributes()) {
        if (attribute.key() == key && attribute.value().string_value() == value) {
          return true;
        }
      }
    }
    return false;
  };

  for (const auto& resourceMetric : proto.resource_metrics()) {
    for (const auto& scopeMetric : resourceMetric.scope_metrics()) {
      for (const auto& metric : scopeMetric.metrics()) {
        if (metric.name() != metricName) {
          continue;
        }
        if ((metric.has_sum() && pointsContainLabel(metric.sum().data_points())) ||
            (metric.has_gauge() && pointsContainLabel(metric.gauge().data_points())) ||
            (metric.has_histogram() && pointsContainLabel(metric.histogram().data_points()))) {
          return true;
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
  telemetryCfg.exportInterval = std::chrono::milliseconds{200};  // Fast export for test
  telemetryCfg.exportTimeout = std::chrono::milliseconds{199};   // Must be < exportInterval

  HttpServerConfig serverCfg;
  serverCfg.withTelemetryConfig(telemetryCfg);
  serverCfg.enableKeepAlive = false;

  test::TestServer server(serverCfg);
  server.router().setDefault([](const HttpRequestView&) { return HttpResponse("otel-ok"); });

  const auto response = test::simpleGet(server.port(), "/otel");
  ASSERT_FALSE(response.empty());
  EXPECT_TRUE(response.contains("otel-ok"));

  // Collect requests until we have both trace and metrics exports or timeout. Both are exported periodically by
  // background threads, in any order: several metrics exports may come before the trace one.
  vector<test::CapturedOtlpRequest> captured;
  bool traceCaptured = false;
  bool metricsCaptured = false;
  const auto deadline = std::chrono::steady_clock::now() + 3s;  // NOLINT(misc-include-cleaner)
  while ((!traceCaptured || !metricsCaptured) && std::chrono::steady_clock::now() < deadline) {
    try {
      captured.emplace_back(collector.waitForRequest(500ms));  // NOLINT(misc-include-cleaner)
      traceCaptured |= captured.back().path == "/v1/traces";
      metricsCaptured |= captured.back().path == "/v1/metrics";
    } catch (const std::exception&) {
      log::error("timed out waiting for a single request; loop and check overall deadline");
    }
  }

  const test::CapturedOtlpRequest* traceReq = nullptr;
  const test::CapturedOtlpRequest* metricsReq = nullptr;
  for (const auto& req : captured) {
    if (req.path == "/v1/traces" && traceReq == nullptr) {
      traceReq = &req;
    } else if (req.path == "/v1/metrics" && metricsReq == nullptr) {
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

  // The single span is exported once. Metrics are exported periodically, so later metrics exports may be pending.
  for (const auto& req : collector.drain()) {
    EXPECT_NE(req.path, "/v1/traces");
  }
}

TEST(OpenTelemetryEndToEnd, EmitsPerMeasurementLabels) {
  test::OtlpTestCollector collector;

  TelemetryConfig cfg;
  cfg.otelEnabled = true;
  cfg.withEndpoint(collector.endpointForTraces());
  cfg.withServiceName("aeronet-label-e2e");
  cfg.exportInterval = 50ms;
  cfg.exportTimeout = 49ms;

  tracing::TelemetryContext telemetry(cfg);
  const MetricLabel labels[]{
      {"protocol", "h2"},
      {"frame.type", "headers"},
  };
  telemetry.counterAdd("aeronet.test.labeled", 1UL, labels);
  telemetry.gauge("aeronet.test.labeled_gauge", 2, labels);
  telemetry.histogram("aeronet.test.labeled_histogram", 3.0, labels);
  telemetry.timing("aeronet.test.labeled_timing", 4ms, labels);

  bool counterProtocolFound = false;
  bool counterFrameTypeFound = false;
  bool gaugeFound = false;
  bool histogramFound = false;
  bool timingFound = false;
  const auto deadline = std::chrono::steady_clock::now() + 2s;  // NOLINT(misc-include-cleaner)
  while (!(counterProtocolFound && counterFrameTypeFound && gaugeFound && histogramFound && timingFound) &&
         std::chrono::steady_clock::now() < deadline) {
    try {
      const auto request = collector.waitForRequest(250ms);
      if (request.path != "/v1/metrics") {
        continue;
      }

      ::opentelemetry::proto::collector::metrics::v1::ExportMetricsServiceRequest proto;
      ASSERT_TRUE(proto.ParseFromString(request.body));
      counterProtocolFound |= MetricsContainLabel(proto, "aeronet.test.labeled", "protocol", "h2");
      counterFrameTypeFound |= MetricsContainLabel(proto, "aeronet.test.labeled", "frame.type", "headers");
      gaugeFound |= MetricsContainLabel(proto, "aeronet.test.labeled_gauge", "protocol", "h2");
      histogramFound |= MetricsContainLabel(proto, "aeronet.test.labeled_histogram", "protocol", "h2");
      timingFound |= MetricsContainLabel(proto, "aeronet.test.labeled_timing", "protocol", "h2");
    } catch (const std::exception& ex) {
      // Periodic exports are asynchronous. Keep polling until the overall deadline.
      log::warn("timed out waiting for a single request; loop and check overall deadline: {}", ex.what());
    }
  }

  EXPECT_TRUE(counterProtocolFound);
  EXPECT_TRUE(counterFrameTypeFound);
  EXPECT_TRUE(gaugeFound);
  EXPECT_TRUE(histogramFound);
  EXPECT_TRUE(timingFound);
}

TEST(OpenTelemetryEndToEnd, EmitsMiddlewareSpanAttributes) {
  test::OtlpTestCollector collector;

  TelemetryConfig telemetryCfg;
  telemetryCfg.otelEnabled = true;
  telemetryCfg.withEndpoint(collector.endpointForTraces());
  telemetryCfg.withServiceName("aeronet-mw-e2e");
  telemetryCfg.withSampleRate(1.0);
  telemetryCfg.exportInterval = std::chrono::milliseconds{200};
  telemetryCfg.exportTimeout = std::chrono::milliseconds{199};

  HttpServerConfig serverCfg;
  serverCfg.withTelemetryConfig(telemetryCfg);
  serverCfg.enableKeepAlive = false;

  test::TestServer server(serverCfg);
  server.router().addRequestMiddleware([](HttpRequestView&) { return MiddlewareResult::Continue(); });
  server.router().addResponseMiddleware([](const HttpRequestView&, HttpResponse&) {});
  server.router().setDefault([](const HttpRequestView&) { return HttpResponse("mw-ok"); });

  const auto response = test::simpleGet(server.port(), "/mw");
  ASSERT_FALSE(response.empty());

  bool sawMiddlewareSpan = false;
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!sawMiddlewareSpan && std::chrono::steady_clock::now() < deadline) {
    try {
      auto req = collector.waitForRequest(500ms);
      if (req.path != "/v1/traces") {
        continue;
      }
      ::opentelemetry::proto::collector::trace::v1::ExportTraceServiceRequest proto;
      ASSERT_TRUE(proto.ParseFromString(req.body));
      for (const auto& rs : proto.resource_spans()) {
        for (const auto& ss : rs.scope_spans()) {
          for (const auto& span : ss.spans()) {
            if (span.name() == "aeronet.middleware") {
              sawMiddlewareSpan = true;
            }
          }
        }
      }
    } catch (const std::exception&) { /* keep polling */
      log::error("caught exception in ttest EmitsMiddlewareSpanAttributes");
    }
  }
  EXPECT_TRUE(sawMiddlewareSpan);
}

// Spans end on the event loop thread. Exporting them there would stall all the connections of the server until the
// collector answers - here until the exporter timeout (10 s by default) after each request.
TEST(OpenTelemetryEndToEnd, UnresponsiveCollectorDoesNotDelayRequests) {
  UnresponsiveCollector collector;

  TelemetryConfig telemetryCfg;
  telemetryCfg.otelEnabled = true;
  telemetryCfg.withEndpoint(collector.endpointForTraces());

  HttpServerConfig serverCfg;
  serverCfg.withTelemetryConfig(telemetryCfg);
  serverCfg.enableKeepAlive = false;

  test::TestServer server(serverCfg);
  server.router().setDefault([](const HttpRequestView&) { return HttpResponse("otel-ok"); });

  const auto start = std::chrono::steady_clock::now();
  for (int requestPos = 0; requestPos < 3; ++requestPos) {
    EXPECT_TRUE(test::simpleGet(server.port(), "/otel").contains("otel-ok"));
  }
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);

  // Reset the connections of the exporters before the server flushes its telemetry on destruction, which would
  // otherwise wait for the exporter timeouts.
  collector.close();
}

}  // namespace aeronet