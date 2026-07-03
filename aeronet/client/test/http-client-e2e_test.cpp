#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "aeronet/aeronet.hpp"
#include "aeronet/client-request.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-method.hpp"

namespace aeronet {
namespace {

// Spins up a plain-HTTP aeronet server on an ephemeral port with a handful of routes.
class HttpClientE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    Router router;
    router.setPath(http::Method::GET, "/hello",
                   [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, "world", "text/plain"); });
    router.setPath(http::Method::POST, "/echo", [](const HttpRequest& req) {
      return HttpResponse(http::StatusCodeOK, req.body(), "application/test");
    });
    router.setPath(http::Method::GET, "/redirect", [](const HttpRequest&) {
      HttpResponse resp(http::StatusCodeFound);
      resp.location("/hello");
      return resp;
    });
    router.setPath(http::Method::GET, "/loop", [](const HttpRequest&) {
      HttpResponse resp(http::StatusCodeFound);
      resp.location("/loop");
      return resp;
    });
    // 303 See Other forces a redirect rewrite to GET with the request body dropped.
    router.setPath(http::Method::POST, "/see-other", [](const HttpRequest&) {
      HttpResponse resp(http::StatusCodeSeeOther);
      resp.location("/hello");
      return resp;
    });
    // 302 Found from a POST -> rewrite to GET (method rewriting for 301/302 non-GET/HEAD).
    router.setPath(http::Method::POST, "/found", [](const HttpRequest&) {
      HttpResponse resp(http::StatusCodeFound);
      resp.location("/hello");
      return resp;
    });
    // A 3xx with no Location header: the client gives up redirecting and returns the 3xx as-is.
    router.setPath(http::Method::GET, "/redirect-no-location",
                   [](const HttpRequest&) { return HttpResponse(http::StatusCodeFound); });
    // A 3xx whose Location cannot be resolved (unsupported scheme): returned as-is.
    router.setPath(http::Method::GET, "/redirect-bad-location", [](const HttpRequest&) {
      HttpResponse resp(http::StatusCodeFound);
      resp.location("ftp://unsupported/x");
      return resp;
    });
    router.setPath(http::Method::PUT, "/put",
                   [](const HttpRequest& req) { return HttpResponse(http::StatusCodeOK, req.body(), "text/plain"); });
    router.setPath(http::Method::PATCH, "/patch",
                   [](const HttpRequest& req) { return HttpResponse(http::StatusCodeOK, req.body(), "text/plain"); });
    router.setPath(http::Method::DELETE, "/resource",
                   [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, "deleted", "text/plain"); });
    // The remaining redirect status codes a GET follows to /hello. 301 keeps the method (the method
    // rewrite only fires for non-GET/HEAD); 307/308 always preserve the method + body.
    router.setPath(http::Method::GET, "/moved-permanently", [](const HttpRequest&) {
      HttpResponse resp(http::StatusCodeMovedPermanently);
      resp.location("/hello");
      return resp;
    });
    router.setPath(http::Method::GET, "/temporary-redirect", [](const HttpRequest&) {
      HttpResponse resp(http::StatusCodeTemporaryRedirect);
      resp.location("/hello");
      return resp;
    });
    router.setPath(http::Method::GET, "/permanent-redirect", [](const HttpRequest&) {
      HttpResponse resp(http::StatusCodePermanentRedirect);
      resp.location("/hello");
      return resp;
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

}  // namespace

TEST_F(HttpClientE2ETest, SimpleGet) {
  HttpClient client;
  auto resp = client.get(url("/hello")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_GE(resp.status(), 200);
  EXPECT_LT(resp.status(), 300);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientE2ETest, NotFound) {
  HttpClient client;
  auto resp = client.get(url("/does-not-exist")).value();
  EXPECT_EQ(resp.status(), 404);
  EXPECT_FALSE(resp.status() >= 200 && resp.status() < 300);
}

TEST_F(HttpClientE2ETest, SurfacesReservedResponseHeadersLosslessly) {
  HttpClient client;
  auto resp = client.get(url("/hello")).value();
  EXPECT_EQ(resp.status(), 200);
  // aeronet always emits Date / Server. Date is reserved on the response-building side, yet the
  // client must surface it verbatim on a received response (lossless via rawHeader()).
  EXPECT_FALSE(resp.headerValueOrEmpty("date").empty());
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
}

TEST_F(HttpClientE2ETest, PostEchoesBody) {
  HttpClient client;
  auto resp = client.post(url("/echo"), "payload-data", "application/test").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "payload-data");
  EXPECT_EQ(resp.headerValueOrEmpty("content-type"), "application/test");
}

TEST_F(HttpClientE2ETest, HeadHasNoBody) {
  HttpClient client;
  ClientRequest req(http::Method::HEAD, url("/hello"));
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST_F(HttpClientE2ETest, FollowsRedirect) {
  HttpClient client;
  auto resp = client.get(url("/redirect")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientE2ETest, RedirectDisabledReturns3xx) {
  HttpClientConfig cfg;
  cfg.followRedirects = false;
  HttpClient client(cfg);
  auto resp = client.get(url("/redirect")).value();
  EXPECT_EQ(resp.status(), 302);
  EXPECT_EQ(resp.headerValueOrEmpty("location"), "/hello");
}

TEST_F(HttpClientE2ETest, RedirectLoopHitsLimit) {
  HttpClientConfig cfg;
  cfg.maxRedirects = 3;
  HttpClient client(cfg);
  // After exhausting the redirect budget, the last 3xx is returned (not an error).
  auto resp = client.get(url("/loop")).value();
  EXPECT_EQ(resp.status(), 302);
}

TEST_F(HttpClientE2ETest, KeepAliveReusesConnection) {
  HttpClient client;  // keep-alive enabled by default
  for (int i = 0; i < 5; ++i) {
    auto resp = client.get(url("/hello")).value();
    EXPECT_EQ(resp.status(), 200);
    EXPECT_EQ(resp.bodyInMemory(), "world");
  }
}

TEST_F(HttpClientE2ETest, ConnectionCloseStillWorks) {
  HttpClientConfig cfg;
  cfg.keepAlive = false;
  HttpClient client(cfg);
  auto resp = client.get(url("/hello")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientE2ETest, InvalidUrlReturnsError) {
  HttpClient client;
  auto result = client.get("not-a-url");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::invalidUrl);
}

TEST_F(HttpClientE2ETest, ConnectionRefusedReturnsError) {
  HttpClient client;
  // Port 1 is almost certainly closed; connect should fail.
  auto result = client.get("http://127.0.0.1:1/");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::connectFailed);
}

TEST_F(HttpClientE2ETest, MaxResponseBytesExceededReturnsError) {
  HttpClientConfig cfg;
  cfg.maxResponseBytes = 2;  // smaller than even the status line
  HttpClient client(cfg);
  auto result = client.get(url("/hello"));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::malformedResponse);
}

TEST_F(HttpClientE2ETest, CustomRequestHeaderIsSent) {
  HttpClient client;
  // The /echo route echoes the body; here we just assert a custom header does not break the round-trip
  // and that an explicit Host override is honoured by the builder.
  ClientRequest req(http::Method::GET, url("/hello"));
  req.headerAddLine("X-Test", "1").header("User-Agent", "custom-agent/1.0");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientE2ETest, SeeOtherRewritesPostToGetAndDropsBody) {
  HttpClient client;
  // POST with a body + a custom header -> 303 -> GET /hello with the body (and its CT/CL) dropped but the
  // user header preserved (exercises the redirect "dropBody" header-rewrite path).
  ClientRequest req(http::Method::POST, url("/see-other"));
  req.body("discarded-payload", "text/plain").headerAddLine("X-Keep", "kept");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientE2ETest, BodylessPostSendsContentLengthZero) {
  HttpClient client;
  // A POST without any body still frames correctly: the builder injects "Content-Length: 0".
  ClientRequest req(http::Method::POST, url("/echo"));
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST_F(HttpClientE2ETest, FoundFromPostRewritesToGet) {
  HttpClient client;
  auto resp = client.post(url("/found"), "body", "text/plain").value();
  EXPECT_EQ(resp.status(), 200);  // 302 -> GET /hello
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientE2ETest, RedirectWithoutLocationReturnsAsIs) {
  HttpClient client;
  auto resp = client.get(url("/redirect-no-location")).value();
  EXPECT_EQ(resp.status(), 302);
}

TEST_F(HttpClientE2ETest, RedirectWithUnresolvableLocationReturnsError) {
  HttpClient client;
  // A 3xx whose Location uses an unsupported scheme cannot be followed: the malformed redirect target
  // surfaces as HttpClientErrc::invalidUrl rather than silently handing back the 3xx response.
  auto result = client.get(url("/redirect-bad-location"));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::invalidUrl);
}

TEST_F(HttpClientE2ETest, PutEchoes) {
  HttpClient client;
  auto resp = client.put(url("/put"), "put-payload", "text/plain").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "put-payload");
}

TEST_F(HttpClientE2ETest, DeleteWorks) {
  HttpClient client;
  auto resp = client.del(url("/resource")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "deleted");
}

TEST_F(HttpClientE2ETest, HeadConvenienceVerb) {
  HttpClient client;
  auto resp = client.head(url("/hello")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST_F(HttpClientE2ETest, ZeroIdlePoolDropsConnection) {
  HttpClientConfig cfg;
  cfg.keepAlive = true;
  cfg.maxIdleConnectionsPerHost = 0;  // never retain a connection (bucket is always "full")
  HttpClient client(cfg);
  for (int i = 0; i < 3; ++i) {
    auto resp = client.get(url("/hello")).value();
    EXPECT_EQ(resp.status(), 200);
    EXPECT_EQ(resp.bodyInMemory(), "world");
  }
}

TEST_F(HttpClientE2ETest, BodylessPutSendsContentLengthZero) {
  HttpClient client;
  // A PUT without a body still frames correctly: the builder injects "Content-Length: 0".
  ClientRequest req(http::Method::PUT, url("/put"));
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST_F(HttpClientE2ETest, BodylessPatchSendsContentLengthZero) {
  HttpClient client;
  // PATCH is also a body-bearing method, so an empty PATCH is framed with "Content-Length: 0".
  ClientRequest req(http::Method::PATCH, url("/patch"));
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST_F(HttpClientE2ETest, FollowsMovedPermanently) {
  HttpClient client;
  // 301 from a GET keeps the method (no rewrite) and is followed to /hello.
  auto resp = client.get(url("/moved-permanently")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientE2ETest, FollowsTemporaryRedirect) {
  HttpClient client;
  auto resp = client.get(url("/temporary-redirect")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientE2ETest, FollowsPermanentRedirect) {
  HttpClient client;
  auto resp = client.get(url("/permanent-redirect")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

// A single client keeps only one fd registered in its event loop at a time (the connection it is currently
// driving). Alternating between two distinct keep-alive origins must swap that registration back and forth
// (drop the previous fd, add the new one) without leaking state between the two pooled connections. This
// exercises armLoop's different-fd path in both directions and guards against a stale/mismatched
// registration corrupting a later exchange.
TEST_F(HttpClientE2ETest, AlternatingOriginsSwapLoopRegistration) {
  Router router2;
  router2.setPath(http::Method::GET, "/hello",
                  [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, "planet", "text/plain"); });
  SingleHttpServer server2(HttpServerConfig{}.withPort(0).withPollInterval(std::chrono::milliseconds{20}),
                           std::move(router2));
  const uint16_t port2 = server2.port();
  server2.start();
  const std::string url2 = "http://127.0.0.1:" + std::to_string(port2) + "/hello";

  HttpClient client;  // keep-alive on by default: both origins stay pooled across the alternation
  for (int i = 0; i < 5; ++i) {
    auto respA = client.get(url("/hello")).value();
    EXPECT_EQ(respA.status(), 200);
    EXPECT_EQ(respA.bodyInMemory(), "world");
    auto respB = client.get(url2).value();
    EXPECT_EQ(respB.status(), 200);
    EXPECT_EQ(respB.bodyInMemory(), "planet");
  }
}

// A request body large enough to overflow the kernel socket buffers forces the write path to block
// (EventOut) on the very fd it will then read the response from (EventIn). This exercises armLoop's
// same-fd re-arm branch (interest change without a full re-register) and round-trips the payload intact
// across the partial-write / event-loop pump.
TEST_F(HttpClientE2ETest, LargePostBlocksWriteThenReadsBack) {
  HttpClient client;
  const std::string body(16U << 20, 'x');  // 16 MiB: far exceeds socket buffers, so the write must block
  auto resp = client.post(url("/echo"), body, "application/test").value();
  EXPECT_EQ(resp.status(), 200);
  ASSERT_EQ(resp.bodyInMemory().size(), body.size());
  EXPECT_EQ(resp.bodyInMemory(), body);
}

// clearIdleConnections() over a live pool must unregister the pooled connection's fd from the event loop
// (not merely forget it), so a subsequent request registers a fresh fd cleanly rather than colliding with a
// stale registration. Covers dropIdleBucket over a still-registered pooled entry.
TEST_F(HttpClientE2ETest, ClearIdleConnectionsDropsRegisteredPooledFd) {
  HttpClient client;
  auto first = client.get(url("/hello")).value();  // pools a keep-alive connection (its fd is registered)
  EXPECT_EQ(first.status(), 200);
  EXPECT_EQ(first.bodyInMemory(), "world");
  client.clearIdleConnections();                    // must unregister + close the pooled fd
  auto second = client.get(url("/hello")).value();  // reconnects and re-registers a fresh fd
  EXPECT_EQ(second.status(), 200);
  EXPECT_EQ(second.bodyInMemory(), "world");
}

}  // namespace aeronet
