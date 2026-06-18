// Integration tests that drive a full aeronet server through the aeronet HttpClient.
// This exercises both ends of the library against each other (request building, chunked
// streaming, keep-alive reuse, query/headers) over real sockets.
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "aeronet/aeronet.hpp"
#include "aeronet/client-request.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-method.hpp"

namespace aeronet {
namespace {

class HttpClientIntegration : public ::testing::Test {
 protected:
  void SetUp() override {
    Router router;
    router.setPath(http::Method::GET, "/echo-query", [](const HttpRequest& req) {
      return HttpResponse(http::StatusCodeOK, std::string(req.queryParamValueOrEmpty("msg")), "text/plain");
    });
    router.setPath(http::Method::POST, "/upload", [](const HttpRequest& req) {
      return HttpResponse(http::StatusCodeOK, std::to_string(req.body().size()), "text/plain");
    });
    router.setPath(http::Method::GET, "/headers", [](const HttpRequest&) {
      HttpResponse resp(http::StatusCodeOK, "ok", "text/plain");
      resp.headerAddLine("X-Custom", "custom-value");
      return resp;
    });
    // Streaming handler -> Transfer-Encoding: chunked on the wire.
    router.setPath(http::Method::GET, "/stream", [](const HttpRequest&, HttpResponseWriter& writer) {
      writer.status(http::StatusCodeOK);
      writer.contentType("text/plain");
      writer.writeBody("alpha-");
      writer.writeBody("beta-");
      writer.writeBody("gamma");
      writer.end();
    });

    _server = std::make_unique<SingleHttpServer>(HttpServerConfig{}
                                                     .withPort(0)
                                                     .withKeepAliveTimeout(std::chrono::milliseconds{200})
                                                     .withPollInterval(std::chrono::milliseconds{20}),
                                                 std::move(router));
    _port = _server->port();
    _server->start();
  }

  void TearDown() override { _server.reset(); }

  [[nodiscard]] std::string url(std::string_view path) const {
    return "http://127.0.0.1:" + std::to_string(_port) + std::string(path);
  }

  std::unique_ptr<SingleHttpServer> _server;
  uint16_t _port{0};
};

TEST_F(HttpClientIntegration, QueryParamRoundTrip) {
  HttpClient client;
  auto resp = client.get(url("/echo-query?msg=hello-world")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "hello-world");
}

TEST_F(HttpClientIntegration, LargeBodyUpload) {
  HttpClient client;
  std::string payload(256UL * 1024UL, 'Z');  // 256 KiB, exercises multi-write / partial writes
  auto resp = client.post(url("/upload"), payload, "application/octet-stream").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), std::to_string(payload.size()));
}

TEST_F(HttpClientIntegration, ReadsCustomResponseHeader) {
  HttpClient client;
  auto resp = client.get(url("/headers")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.headerValueOrEmpty("x-custom"), "custom-value");  // case-insensitive lookup
  EXPECT_EQ(resp.bodyInMemory(), "ok");
}

TEST_F(HttpClientIntegration, DecodesChunkedStreamingResponse) {
  HttpClient client;
  auto resp = client.get(url("/stream")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "alpha-beta-gamma");  // chunked framing de-framed by the client
}

TEST_F(HttpClientIntegration, KeepAliveAcrossManyRequests) {
  HttpClient client;
  for (int i = 0; i < 25; ++i) {
    auto resp = client.get(url("/echo-query?msg=k")).value();
    ASSERT_EQ(resp.status(), 200) << "iteration " << i;
    ASSERT_EQ(resp.bodyInMemory(), "k");
  }
}

TEST_F(HttpClientIntegration, MixedMethodsReuseClient) {
  HttpClient client;
  EXPECT_EQ(client.get(url("/echo-query?msg=a")).value().bodyInMemory(), "a");
  EXPECT_EQ(client.post(url("/upload"), "1234", "text/plain").value().bodyInMemory(), "4");
  EXPECT_EQ(client.get(url("/stream")).value().bodyInMemory(), "alpha-beta-gamma");
}

TEST_F(HttpClientIntegration, IdlePooledConnectionExpires) {
  HttpClientConfig cfg;
  cfg.withKeepAliveTimeout(std::chrono::milliseconds{30});  // expire pooled connections quickly
  HttpClient client(cfg);

  EXPECT_EQ(client.get(url("/echo-query?msg=first")).value().bodyInMemory(), "first");
  std::this_thread::sleep_for(std::chrono::milliseconds{80});  // exceed the client idle limit
  // The pooled connection is now considered stale and dropped; a fresh one transparently serves this.
  EXPECT_EQ(client.get(url("/echo-query?msg=second")).value().bodyInMemory(), "second");
}

}  // namespace
}  // namespace aeronet
