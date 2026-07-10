#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/aeronet.hpp"
#include "aeronet/client-protocol.hpp"
#include "aeronet/client-request.hpp"
#include "aeronet/close-native-handle.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client-exception.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/retry-config.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/tls-config.hpp"

#ifdef AERONET_POSIX
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#elifdef AERONET_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include <filesystem>
#include <fstream>

#include "aeronet/test-tls-helper.hpp"
#include "aeronet/tls-config.hpp"
#endif

namespace aeronet {

namespace {

// Reflects a handful of request headers back so a test can observe exactly what the request builder put
// on the wire (which framing headers it injected, and which user-supplied ones suppressed the injection).
HttpResponse ReflectRequestHeaders(const HttpRequest& req) {
  auto resp = req.makeResponse(http::StatusCodeOK, "reflect", "text/plain");
  resp.header("echo-host", req.headerValueOrEmpty("host"));
  resp.header("echo-ua-present", req.hasHeader("user-agent") ? "yes" : "no");
  return resp;
}

test::TestServer CreateTestServer() {
  // Generous server keep-alive idle timeout: a short one reaps a freshly-accepted connection whose client
  // is descheduled (heavy parallel-test load) between the TCP handshake and sending its request, which
  // surfaces as a rare terminal failure of the in-flight request. See the compression e2e test for detail.

  test::TestServer testServer(HttpServerConfig{}
                                  .withPort(0)
                                  .withKeepAliveTimeout(std::chrono::seconds{5})
                                  .withPollInterval(std::chrono::milliseconds{20}));

  auto routerProxy = testServer.router();
  routerProxy.setPath(http::Method::GET | http::Method::POST, "/reflect", ReflectRequestHeaders);
  routerProxy.setPath(http::Method::GET, "/hello", [](const HttpRequest& req) {
    return req.makeResponse(http::StatusCodeOK, "world", "text/plain");
  });
  routerProxy.setPath(http::Method::POST, "/echo", [](const HttpRequest& req) {
    return req.makeResponse(http::StatusCodeOK, req.body(), "application/test");
  });
  routerProxy.setPath(http::Method::GET, "/redirect", [](const HttpRequest& req) {
    auto resp = req.makeResponse(http::StatusCodeFound);
    resp.location("/hello");
    return resp;
  });
  routerProxy.setPath(http::Method::GET, "/loop", [](const HttpRequest& req) {
    auto resp = req.makeResponse(http::StatusCodeFound);
    resp.location("/loop");
    return resp;
  });
  // 303 See Other forces a redirect rewrite to GET with the request body dropped.
  routerProxy.setPath(http::Method::POST, "/see-other", [](const HttpRequest& req) {
    auto resp = req.makeResponse(http::StatusCodeSeeOther);
    resp.location("/hello");
    return resp;
  });
  // 302 Found from a POST -> rewrite to GET (method rewriting for 301/302 non-GET/HEAD).
  routerProxy.setPath(http::Method::POST, "/found", [](const HttpRequest& req) {
    auto resp = req.makeResponse(http::StatusCodeFound);
    resp.location("/hello");
    return resp;
  });
  // A 3xx with no Location header: the client gives up redirecting and returns the 3xx as-is.
  routerProxy.setPath(http::Method::GET, "/redirect-no-location",
                      [](const HttpRequest& req) { return req.makeResponse(http::StatusCodeFound); });
  // A 3xx whose Location cannot be resolved (unsupported scheme): returned as-is.
  routerProxy.setPath(http::Method::GET, "/redirect-bad-location", [](const HttpRequest& req) {
    auto resp = req.makeResponse(http::StatusCodeFound);
    resp.location("ftp://unsupported/x");
    return resp;
  });
  routerProxy.setPath(http::Method::PUT, "/put", [](const HttpRequest& req) {
    return req.makeResponse(http::StatusCodeOK, req.body(), "text/plain");
  });
  routerProxy.setPath(http::Method::PATCH, "/patch", [](const HttpRequest& req) {
    return req.makeResponse(http::StatusCodeOK, req.body(), "text/plain");
  });
  routerProxy.setPath(http::Method::DELETE, "/resource", [](const HttpRequest& req) {
    return req.makeResponse(http::StatusCodeOK, "deleted", "text/plain");
  });
  // The remaining redirect status codes a GET follows to /hello. 301 keeps the method (the method
  // rewrite only fires for non-GET/HEAD); 307/308 always preserve the method + body.
  routerProxy.setPath(http::Method::GET, "/moved-permanently", [](const HttpRequest& req) {
    auto resp = req.makeResponse(http::StatusCodeMovedPermanently);
    resp.location("/hello");
    return resp;
  });
  routerProxy.setPath(http::Method::GET, "/temporary-redirect", [](const HttpRequest& req) {
    auto resp = req.makeResponse(http::StatusCodeTemporaryRedirect);
    resp.location("/hello");
    return resp;
  });
  routerProxy.setPath(http::Method::GET, "/permanent-redirect", [](const HttpRequest& req) {
    auto resp = req.makeResponse(http::StatusCodePermanentRedirect);
    resp.location("/hello");
    return resp;
  });

  return testServer;
}

test::TestServer ts = CreateTestServer();
auto port = ts.port();

// Spins up a plain-HTTP aeronet server on an ephemeral port with a handful of routes.
class HttpClientE2ETest : public ::testing::Test {
 protected:
  [[nodiscard]] static std::string Url(std::string_view path) {
    return "http://127.0.0.1:" + std::to_string(port) + std::string(path);
  }
};

}  // namespace

TEST_F(HttpClientE2ETest, SimpleGet) {
  HttpClient client;
  auto resp = client.get(Url("/hello")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_GE(resp.status(), 200);
  EXPECT_LT(resp.status(), 300);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientE2ETest, NotFound) {
  HttpClient client;
  auto resp = client.get(Url("/does-not-exist")).value();
  EXPECT_EQ(resp.status(), 404);
  EXPECT_FALSE(resp.status() >= 200 && resp.status() < 300);
}

TEST_F(HttpClientE2ETest, SurfacesReservedResponseHeadersLosslessly) {
  HttpClient client;
  auto resp = client.get(Url("/hello")).value();
  EXPECT_EQ(resp.status(), 200);
  // aeronet always emits Date / Server. Date is reserved on the response-building side, yet the
  // client must surface it verbatim on a received response (lossless via rawHeader()).
  EXPECT_FALSE(resp.headerValueOrEmpty("date").empty());
  EXPECT_EQ(resp.headerValueOrEmpty("server"), "aeronet");
}

TEST_F(HttpClientE2ETest, PostEchoesBody) {
  HttpClient client;
  auto resp = client.post(Url("/echo"), "payload-data", "application/test").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "payload-data");
  EXPECT_EQ(resp.headerValueOrEmpty("content-type"), "application/test");
}

TEST_F(HttpClientE2ETest, HeadHasNoBody) {
  HttpClient client;
  ClientRequest req(http::Method::HEAD, Url("/hello"));
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST_F(HttpClientE2ETest, FollowsRedirect) {
  HttpClient client;
  auto resp = client.get(Url("/redirect")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientE2ETest, RedirectDisabledReturns3xx) {
  HttpClientConfig cfg;
  cfg.followRedirects = false;
  HttpClient client(cfg);
  auto resp = client.get(Url("/redirect")).value();
  EXPECT_EQ(resp.status(), 302);
  EXPECT_EQ(resp.headerValueOrEmpty("location"), "/hello");
}

TEST_F(HttpClientE2ETest, RedirectLoopHitsLimit) {
  HttpClientConfig cfg;
  cfg.maxRedirects = 3;
  HttpClient client(cfg);
  // After exhausting the redirect budget, the last 3xx is returned (not an error).
  auto resp = client.get(Url("/loop")).value();
  EXPECT_EQ(resp.status(), 302);
}

TEST_F(HttpClientE2ETest, KeepAliveReusesConnection) {
  HttpClient client;  // keep-alive enabled by default
  for (int i = 0; i < 5; ++i) {
    auto resp = client.get(Url("/hello")).value();
    EXPECT_EQ(resp.status(), 200);
    EXPECT_EQ(resp.bodyInMemory(), "world");
  }
}

TEST_F(HttpClientE2ETest, ConnectionCloseStillWorks) {
  HttpClientConfig cfg;
  cfg.keepAlive = false;
  HttpClient client(cfg);
  auto resp = client.get(Url("/hello")).value();
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
  auto result = client.get(Url("/hello"));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::malformedResponse);
}

TEST_F(HttpClientE2ETest, CustomRequestHeaderIsSent) {
  HttpClient client;
  // The /echo route echoes the body; here we just assert a custom header does not break the round-trip
  // and that an explicit Host override is honoured by the builder.
  ClientRequest req(http::Method::GET, Url("/hello"));
  req.headerAddLine("X-Test", "1").header("User-Agent", "custom-agent/1.0");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientE2ETest, SeeOtherRewritesPostToGetAndDropsBody) {
  HttpClient client;
  // POST with a body + a custom header -> 303 -> GET /hello with the body (and its CT/CL) dropped but the
  // user header preserved (exercises the redirect "dropBody" header-rewrite path).
  ClientRequest req(http::Method::POST, Url("/see-other"));
  req.body("discarded-payload", "text/plain").headerAddLine("X-Keep", "kept");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientE2ETest, BodylessPostSendsContentLengthZero) {
  HttpClient client;
  // A POST without any body still frames correctly: the builder injects "Content-Length: 0".
  ClientRequest req(http::Method::POST, Url("/echo"));
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST_F(HttpClientE2ETest, FoundFromPostRewritesToGet) {
  HttpClient client;
  auto resp = client.post(Url("/found"), "body", "text/plain").value();
  EXPECT_EQ(resp.status(), 200);  // 302 -> GET /hello
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientE2ETest, RedirectWithoutLocationReturnsAsIs) {
  HttpClient client;
  auto resp = client.get(Url("/redirect-no-location")).value();
  EXPECT_EQ(resp.status(), 302);
}

TEST_F(HttpClientE2ETest, RedirectWithUnresolvableLocationReturnsError) {
  HttpClient client;
  // A 3xx whose Location uses an unsupported scheme cannot be followed: the malformed redirect target
  // surfaces as HttpClientErrc::invalidUrl rather than silently handing back the 3xx response.
  auto result = client.get(Url("/redirect-bad-location"));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::invalidUrl);
}

TEST_F(HttpClientE2ETest, PutEchoes) {
  HttpClient client;
  auto resp = client.put(Url("/put"), "put-payload", "text/plain").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "put-payload");
}

TEST_F(HttpClientE2ETest, DeleteWorks) {
  HttpClient client;
  auto resp = client.del(Url("/resource")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "deleted");
}

TEST_F(HttpClientE2ETest, HeadConvenienceVerb) {
  HttpClient client;
  auto resp = client.head(Url("/hello")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST_F(HttpClientE2ETest, ZeroIdlePoolDropsConnection) {
  HttpClientConfig cfg;
  cfg.keepAlive = true;
  cfg.maxIdleConnectionsPerHost = 0;  // never retain a connection (bucket is always "full")
  HttpClient client(cfg);
  for (int i = 0; i < 3; ++i) {
    auto resp = client.get(Url("/hello")).value();
    EXPECT_EQ(resp.status(), 200);
    EXPECT_EQ(resp.bodyInMemory(), "world");
  }
}

TEST_F(HttpClientE2ETest, BodylessPutSendsContentLengthZero) {
  HttpClient client;
  // A PUT without a body still frames correctly: the builder injects "Content-Length: 0".
  ClientRequest req(http::Method::PUT, Url("/put"));
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST_F(HttpClientE2ETest, BodylessPatchSendsContentLengthZero) {
  HttpClient client;
  // PATCH is also a body-bearing method, so an empty PATCH is framed with "Content-Length: 0".
  ClientRequest req(http::Method::PATCH, Url("/patch"));
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());
}

TEST_F(HttpClientE2ETest, FollowsMovedPermanently) {
  HttpClient client;
  // 301 from a GET keeps the method (no rewrite) and is followed to /hello.
  auto resp = client.get(Url("/moved-permanently")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientE2ETest, FollowsTemporaryRedirect) {
  HttpClient client;
  auto resp = client.get(Url("/temporary-redirect")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "world");
}

TEST_F(HttpClientE2ETest, FollowsPermanentRedirect) {
  HttpClient client;
  auto resp = client.get(Url("/permanent-redirect")).value();
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
    auto respA = client.get(Url("/hello")).value();
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
  auto resp = client.post(Url("/echo"), body, "application/test").value();
  EXPECT_EQ(resp.status(), 200);
  ASSERT_EQ(resp.bodyInMemory().size(), body.size());
  EXPECT_EQ(resp.bodyInMemory(), body);
}

// clearIdleConnections() over a live pool must unregister the pooled connection's fd from the event loop
// (not merely forget it), so a subsequent request registers a fresh fd cleanly rather than colliding with a
// stale registration. Covers dropIdleBucket over a still-registered pooled entry.
TEST_F(HttpClientE2ETest, ClearIdleConnectionsDropsRegisteredPooledFd) {
  HttpClient client;
  auto first = client.get(Url("/hello")).value();  // pools a keep-alive connection (its fd is registered)
  EXPECT_EQ(first.status(), 200);
  EXPECT_EQ(first.bodyInMemory(), "world");
  client.clearIdleConnections();                    // must unregister + close the pooled fd
  auto second = client.get(Url("/hello")).value();  // reconnects and re-registers a fresh fd
  EXPECT_EQ(second.status(), 200);
  EXPECT_EQ(second.bodyInMemory(), "world");
}

// --- Request-builder header injection decisions -----------------------------

// A user-supplied Host header suppresses the auto-injected one and is sent verbatim (the builder emits the
// user's Host rather than deriving it from the URL authority).
TEST_F(HttpClientE2ETest, ExplicitHostHeaderSuppressesInjected) {
  HttpClient client;
  ClientRequest req(http::Method::GET, Url("/reflect"));
  req.header("Host", "custom-authority.example:1234");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.headerValueOrEmpty("echo-host"), "custom-authority.example:1234");
}

// An empty configured User-Agent suppresses the User-Agent header entirely (no empty line emitted).
TEST_F(HttpClientE2ETest, EmptyUserAgentConfigOmitsHeader) {
  HttpClient client(HttpClientConfig{}.withUserAgent(""));
  auto resp = client.get(Url("/reflect")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.headerValueOrEmpty("echo-ua-present"), "no");
}

// With keep-alive off the builder would inject "Connection: close"; a user-supplied Connection header
// suppresses that injection (the user's directive wins). The exchange still completes.
TEST_F(HttpClientE2ETest, ExplicitConnectionHeaderWithKeepAliveOff) {
  HttpClientConfig cfg;
  cfg.keepAlive = false;
  HttpClient client(cfg);
  ClientRequest req(http::Method::GET, Url("/reflect"));
  req.header("Connection", "close");
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "reflect");
}

// A HEAD following a 302 keeps its method: the POST->GET rewrite for 301/302 fires only for methods that
// are neither GET nor HEAD, so a HEAD request is redirected as HEAD (body stays absent).
TEST_F(HttpClientE2ETest, HeadFollowsRedirectWithoutMethodRewrite) {
  HttpClient client;
  ClientRequest req(http::Method::HEAD, Url("/redirect"));  // 302 -> /hello
  auto resp = client.request(req).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());  // HEAD: no body on the final response either
}

// With keep-alive idle expiry disabled (keepAliveTimeout == 0), a pooled connection is reused without the
// staleness/age check firing -- the second request rides the same connection.
TEST_F(HttpClientE2ETest, KeepAliveWithoutExpiryReusesConnection) {
  HttpClient client(HttpClientConfig{}.withKeepAliveTimeout(std::chrono::milliseconds{0}));
  auto first = client.get(Url("/hello")).value();
  EXPECT_EQ(first.status(), 200);
  auto second = client.get(Url("/hello")).value();  // reuses the pooled connection (no expiry check)
  EXPECT_EQ(second.status(), 200);
  EXPECT_EQ(second.bodyInMemory(), "world");
}

namespace {

class HttpClientCacheE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto router = ts.resetRouterAndGet();
    // GET/HEAD counter: body is the running hit count so a cached response is identifiable by its value.
    router.setPath(http::Method::GET | http::Method::HEAD, "/counter", [this](const HttpRequest&) {
      const int nth = _counterHits.fetch_add(1, std::memory_order_relaxed) + 1;
      return HttpResponse(http::StatusCodeOK, std::to_string(nth), "text/plain");
    });
    router.setPath(http::Method::GET, "/error", [this](const HttpRequest&) {
      _errorHits.fetch_add(1, std::memory_order_relaxed);
      return HttpResponse(http::StatusCodeInternalServerError, "err", "text/plain");
    });
    router.setPath(http::Method::GET, "/a", [this](const HttpRequest&) {
      _aHits.fetch_add(1, std::memory_order_relaxed);
      return HttpResponse(http::StatusCodeOK, "a", "text/plain");
    });
    router.setPath(http::Method::GET, "/b", [this](const HttpRequest&) {
      _bHits.fetch_add(1, std::memory_order_relaxed);
      return HttpResponse(http::StatusCodeOK, "b", "text/plain");
    });
    router.setPath(http::Method::POST, "/echo", [this](const HttpRequest& req) {
      _echoHits.fetch_add(1, std::memory_order_relaxed);
      return HttpResponse(http::StatusCodeOK, req.body(), "application/test");
    });
  }

  [[nodiscard]] static std::string Url(std::string_view path) {
    return "http://127.0.0.1:" + std::to_string(port) + std::string(path);
  }

  std::atomic<int> _counterHits{0};
  std::atomic<int> _errorHits{0};
  std::atomic<int> _aHits{0};
  std::atomic<int> _bHits{0};
  std::atomic<int> _echoHits{0};
};

}  // namespace

TEST_F(HttpClientCacheE2ETest, CachesRepeatedGetWithinTtl) {
  HttpClient client(HttpClientConfig{}.withCache(std::chrono::seconds{30}));
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "1");
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "1");  // served from cache
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "1");
  EXPECT_EQ(_counterHits.load(), 1);
}

TEST_F(HttpClientCacheE2ETest, DisabledByDefault) {
  HttpClient client;  // no cache configured
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "1");
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "2");
  EXPECT_EQ(_counterHits.load(), 2);
}

TEST_F(HttpClientCacheE2ETest, RefetchesAfterTtlExpiry) {
  HttpClient client(HttpClientConfig{}.withCache(std::chrono::milliseconds{200}));
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "1");
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "1");  // still fresh -> cached
  std::this_thread::sleep_for(std::chrono::milliseconds{400});         // exceed the TTL
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "2");  // stale -> refreshed
  EXPECT_EQ(_counterHits.load(), 2);
}

TEST_F(HttpClientCacheE2ETest, CachesForeverWithMaxDuration) {
  HttpClient client(HttpClientConfig{}.withCache(HttpClientConfig::Duration::max()));
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "1");
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "1");  // never expires
  EXPECT_EQ(_counterHits.load(), 1);
}

TEST_F(HttpClientCacheE2ETest, NonSuccessResponsesNotCached) {
  HttpClient client(HttpClientConfig{}.withCache(std::chrono::seconds{30}));
  EXPECT_EQ(client.get(Url("/error")).value().status(), 500);
  EXPECT_EQ(client.get(Url("/error")).value().status(), 500);
  EXPECT_EQ(_errorHits.load(), 2);  // a 500 is never stored, so every request hits the server
}

TEST_F(HttpClientCacheE2ETest, TransportErrorIsNotCached) {
  HttpClient client(HttpClientConfig{}.withCache(std::chrono::seconds{30}));
  // Port 1 is never listening (privileged, unbound): the eligible GET fails at connect, so the error result
  // is never stored -- a second attempt fails identically rather than being served a cached error.
  static constexpr std::string_view kUnreachable = "http://127.0.0.1:1/counter";
  EXPECT_FALSE(client.get(kUnreachable).has_value());
  EXPECT_FALSE(client.get(kUnreachable).has_value());
}

TEST_F(HttpClientCacheE2ETest, PostIsNotEligible) {
  HttpClient client(HttpClientConfig{}.withCache(std::chrono::seconds{30}));
  EXPECT_EQ(client.post(Url("/echo"), "x", "text/plain").value().bodyInMemory(), "x");
  EXPECT_EQ(client.post(Url("/echo"), "x", "text/plain").value().bodyInMemory(), "x");
  EXPECT_EQ(_echoHits.load(), 2);  // POST is not a cacheable method (default GET|HEAD)
}

TEST_F(HttpClientCacheE2ETest, HeadCachedSeparatelyFromGet) {
  HttpClient client(HttpClientConfig{}.withCache(std::chrono::seconds{30}));
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "1");  // fetch #1
  EXPECT_EQ(client.head(Url("/counter")).value().status(), 200);       // fetch #2 (distinct method key)
  EXPECT_EQ(_counterHits.load(), 2);
  // Both are now cached; repeats contact no server.
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "1");
  (void)client.head(Url("/counter")).value();
  EXPECT_EQ(_counterHits.load(), 2);
}

TEST_F(HttpClientCacheE2ETest, DistinctRequestHeadersAreDistinctEntries) {
  HttpClient client(HttpClientConfig{}.withCache(std::chrono::seconds{30}));
  ClientRequest reqA(http::Method::GET, Url("/counter"));
  reqA.header("X-Tenant", "a");
  ClientRequest reqB(http::Method::GET, Url("/counter"));
  reqB.header("X-Tenant", "b");

  EXPECT_EQ(client.request(reqA).value().bodyInMemory(), "1");
  EXPECT_EQ(client.request(reqB).value().bodyInMemory(), "2");  // different header -> different key
  EXPECT_EQ(client.request(reqA).value().bodyInMemory(), "1");  // reqA served from cache
  EXPECT_EQ(client.request(reqB).value().bodyInMemory(), "2");  // reqB served from cache
  EXPECT_EQ(_counterHits.load(), 2);
}

TEST_F(HttpClientCacheE2ETest, ClearResponseCacheForcesRefetch) {
  HttpClient client(HttpClientConfig{}.withCache(std::chrono::seconds{30}));
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "1");
  client.clearResponseCache();
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "2");  // cache emptied -> refetch
  EXPECT_EQ(_counterHits.load(), 2);
}

TEST_F(HttpClientCacheE2ETest, MaxEntriesEvictsLeastRecentlyRefreshed) {
  HttpClient client(HttpClientConfig{}.withCache(std::chrono::seconds{30}).withCacheMaxEntries(1));
  EXPECT_EQ(client.get(Url("/a")).value().bodyInMemory(), "a");  // caches /a
  EXPECT_EQ(client.get(Url("/b")).value().bodyInMemory(), "b");  // caches /b, evicts /a (capacity 1)
  EXPECT_EQ(client.get(Url("/a")).value().bodyInMemory(), "a");  // /a was evicted -> refetch
  EXPECT_EQ(_aHits.load(), 2);
  EXPECT_EQ(_bHits.load(), 1);
}

TEST_F(HttpClientCacheE2ETest, ExpiredEntriesPrunedWhenFull) {
  HttpClient client(HttpClientConfig{}.withCache(std::chrono::milliseconds{150}).withCacheMaxEntries(2));
  (void)client.get(Url("/a")).value();
  (void)client.get(Url("/b")).value();                          // cache now full (2 entries)
  std::this_thread::sleep_for(std::chrono::milliseconds{200});  // both entries expire
  (void)client.get(Url("/counter")).value();  // full -> prune drops both expired, then inserts /counter
  // /a and /b were pruned, so re-requesting /a hits the server again.
  (void)client.get(Url("/a")).value();
  EXPECT_EQ(_aHits.load(), 2);
}

TEST_F(HttpClientCacheE2ETest, RestrictingCacheMethodsToGetLeavesHeadUncached) {
  HttpClient client(HttpClientConfig{}.withCache(std::chrono::seconds{30}).withCacheMethods(http::Method::GET));
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "1");
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "1");  // GET is cached
  EXPECT_EQ(_counterHits.load(), 1);
  (void)client.head(Url("/counter")).value();  // HEAD not eligible -> refetch (#2)
  (void)client.head(Url("/counter")).value();  // -> refetch (#3)
  EXPECT_EQ(_counterHits.load(), 3);
}

TEST_F(HttpClientCacheE2ETest, PeriodicPruneKeepsServingHits) {
  // Drive enough cache hits (>= the internal prune interval of 256) to trigger the amortized expiry sweep,
  // and confirm it never evicts a still-fresh entry.
  HttpClient client(HttpClientConfig{}.withCache(std::chrono::seconds{60}));
  EXPECT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "1");  // one network fetch
  for (int i = 0; i < 300; ++i) {
    ASSERT_EQ(client.get(Url("/counter")).value().bodyInMemory(), "1");  // all cache hits
  }
  EXPECT_EQ(_counterHits.load(), 1);
}

namespace {

class HttpClientCompressionE2E : public ::testing::Test {
 protected:
  void SetUp() override {
    auto router = ts.resetRouterAndGet();
    // Echoes the (already server-decompressed) request body verbatim.
    router.setPath(http::Method::POST, "/echo",
                   [](const HttpRequest& req) { return HttpResponse(http::StatusCodeOK, req.body(), "text/plain"); });
    // Returns the size of the (server-decompressed) request body, as a tiny uncompressed response.
    router.setPath(http::Method::POST, "/size", [](const HttpRequest& req) {
      return HttpResponse(http::StatusCodeOK, std::to_string(req.body().size()), "text/plain");
    });
    // Returns the Accept-Encoding header the server received (to observe what the client advertised).
    router.setPath(http::Method::GET, "/accept-encoding", [](const HttpRequest& req) {
      return HttpResponse(http::StatusCodeOK, std::string(req.headerValueOrEmpty("accept-encoding")), "text/plain");
    });
    // A large, highly compressible blob so the server compresses the response.
    router.setPath(http::Method::GET, "/blob",
                   [this](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, _blob, "text/plain"); });

    ts.postConfigUpdate([](HttpServerConfig& cfg) {
      cfg.compression.minBytes = 16;  // compress even small response bodies
    });
  }

  [[nodiscard]] static std::string Url(std::string_view path) {
    return "http://127.0.0.1:" + std::to_string(port) + std::string(path);
  }

  std::string _blob = test::MakePatternedPayload(64UL * 1024UL);
};

}  // namespace

// --- Response decompression -------------------------------------------------

TEST_F(HttpClientCompressionE2E, AutoDecompressesResponseByDefault) {
  HttpClient client;  // decompression on by default; auto-advertises Accept-Encoding
  auto resp = client.get(Url("/blob")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), _blob);                             // transparently decoded
  EXPECT_TRUE(resp.headerValueOrEmpty("content-encoding").empty());  // header dropped after decode
}

TEST_F(HttpClientCompressionE2E, DisabledDecompressionLeavesBodyEncoded) {
  if (test::SupportedEncodings().empty()) {
    GTEST_SKIP() << "no codec compiled in";
  }
  HttpClientConfig cfg;
  cfg.withDecompression(false).withDefaultAcceptEncoding("gzip, br, zstd, deflate");
  HttpClient client(cfg);
  auto resp = client.get(Url("/blob")).value();
  EXPECT_EQ(resp.status(), 200);
  // Pass-through: the server compressed it, the client did not decode it.
  EXPECT_NE(resp.bodyInMemory(), _blob);
  EXPECT_FALSE(resp.headerValueOrEmpty("content-encoding").empty());
  EXPECT_LT(resp.bodyInMemory().size(), _blob.size());
}

// --- Accept-Encoding advertising -------------------------------------------

TEST_F(HttpClientCompressionE2E, AutoAdvertisesSupportedEncodings) {
  HttpClient client;
  auto resp = client.get(Url("/accept-encoding")).value();
  EXPECT_EQ(resp.status(), 200);
  if (test::SupportedEncodings().empty()) {
    EXPECT_TRUE(resp.bodyInMemory().empty());
  } else {
    EXPECT_FALSE(resp.bodyInMemory().empty());  // advertised the codecs we can decode
  }
}

TEST_F(HttpClientCompressionE2E, ExplicitAcceptEncodingOverridesAutoAdvertise) {
  HttpClientConfig cfg;
  cfg.withDefaultAcceptEncoding("identity");
  HttpClient client(cfg);
  auto resp = client.get(Url("/accept-encoding")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "identity");
}

TEST_F(HttpClientCompressionE2E, DisabledDecompressionDoesNotAdvertise) {
  HttpClientConfig cfg;
  cfg.withDecompression(false);  // and no explicit defaultAcceptEncoding
  HttpClient client(cfg);
  auto resp = client.get(Url("/accept-encoding")).value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_TRUE(resp.bodyInMemory().empty());  // nothing advertised
}

// --- Request compression ----------------------------------------------------

TEST_F(HttpClientCompressionE2E, CompressesLargeRequestBody) {
  if (test::SupportedEncodings().empty()) {
    GTEST_SKIP() << "no codec compiled in";
  }
  HttpClientConfig cfg;
  cfg.withRequestCompression(true);
  cfg.requestCompression.codec.minBytes = 16;
  HttpClient client(cfg);

  const std::string payload = test::MakePatternedPayload(48UL * 1024UL);
  // The server decompresses the request body; /size echoes the decompressed length back.
  auto resp = client.post(Url("/size"), payload, "text/plain").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), std::to_string(payload.size()));

  // And a full content echo round-trips byte-for-byte.
  auto echo = client.post(Url("/echo"), payload, "text/plain").value();
  EXPECT_EQ(echo.status(), 200);
  EXPECT_EQ(echo.bodyInMemory(), payload);
}

TEST_F(HttpClientCompressionE2E, IncompressibleRequestBodyFallsBackToPlain) {
  if (test::SupportedEncodings().empty()) {
    GTEST_SKIP() << "no codec compiled in";
  }
  HttpClientConfig cfg;
  cfg.withRequestCompression(true);
  cfg.requestCompression.codec.minBytes = 16;
  HttpClient client(cfg);

  const RawChars random = test::MakeRandomPayload(8192);
  const std::string payload(random.data(), random.size());
  // Compression is not beneficial -> sent uncompressed, server still receives it intact.
  auto resp = client.post(Url("/size"), payload, "application/octet-stream").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), std::to_string(payload.size()));
}

TEST_F(HttpClientCompressionE2E, SmallRequestBodyBelowThresholdIsNotCompressed) {
  HttpClientConfig cfg;
  cfg.withRequestCompression(true);  // default minBytes (1024) keeps tiny bodies uncompressed
  HttpClient client(cfg);
  auto resp = client.post(Url("/echo"), "tiny", "text/plain").value();
  EXPECT_EQ(resp.status(), 200);
  EXPECT_EQ(resp.bodyInMemory(), "tiny");
}

// A bodyless request (GET) with request compression enabled short-circuits the compression path on the
// empty-body guard -- no Content-Encoding is emitted and the request is sent as-is.
TEST_F(HttpClientCompressionE2E, RequestCompressionSkippedForBodylessGet) {
  HttpClientConfig cfg;
  cfg.withRequestCompression(true);
  cfg.requestCompression.codec.minBytes = 1;  // aggressive threshold: only the empty body prevents compression
  HttpClient client(cfg);
  auto resp = client.get(Url("/accept-encoding")).value();
  EXPECT_EQ(resp.status(), 200);
}

// --- Captured vs scattered request write ------------------------------------

TEST_F(HttpClientCompressionE2E, CapturedAndScatteredBodiesBothRoundTrip) {
  HttpClientConfig cfg;
  cfg.withMaxCapturedRequestBodyBytes(1024);  // small bodies captured, large ones scattered
  HttpClient client(cfg);

  auto smallResp = client.post(Url("/echo"), "captured-body", "text/plain").value();  // <= threshold => single write
  EXPECT_EQ(smallResp.bodyInMemory(), "captured-body");

  const std::string big(4096, 'x');  // > threshold => scatter write
  auto largeResp = client.post(Url("/echo"), big, "text/plain").value();
  EXPECT_EQ(largeResp.bodyInMemory(), big);
}

// --- Per-encoding request compression --------------------------------------

TEST_F(HttpClientCompressionE2E, EachSupportedEncodingForRequestBody) {
  const std::string payload = test::MakePatternedPayload(32UL * 1024UL);
  for (const Encoding enc : test::SupportedEncodings()) {
    HttpClientConfig cfg;
    cfg.withRequestCompression(enc);
    cfg.requestCompression.codec.minBytes = 16;
    HttpClient client(cfg);
    auto resp = client.post(Url("/echo"), payload, "text/plain").value();
    EXPECT_EQ(resp.status(), 200) << GetEncodingStr(enc);
    EXPECT_EQ(resp.bodyInMemory(), payload) << GetEncodingStr(enc);
  }
}

TEST(HttpClientErrcTest, EveryCodeHasNonEmptyDescription) {
  constexpr std::array kAll{
      HttpClientErrc::invalidUrl,        HttpClientErrc::connectFailed,       HttpClientErrc::tlsError,
      HttpClientErrc::timeout,           HttpClientErrc::writeError,          HttpClientErrc::connectionClosed,
      HttpClientErrc::malformedResponse, HttpClientErrc::protocolUnsupported, HttpClientErrc::proxyError,
      HttpClientErrc::ioError,
  };
  for (const HttpClientErrc errc : kAll) {
    EXPECT_FALSE(ErrcToStr(errc).empty()) << static_cast<int>(errc);
  }
}

TEST(HttpClientErrcTest, DescriptionsAreStable) {
  EXPECT_EQ(ErrcToStr(HttpClientErrc::invalidUrl), "malformed or unsupported URL");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::connectFailed), "connection failed");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::tlsError), "TLS handshake failed");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::timeout), "operation timed out");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::writeError), "transport write failed");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::connectionClosed), "connection closed before a complete response was received");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::malformedResponse), "malformed or oversized response");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::protocolUnsupported), "negotiated application protocol is not supported");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::proxyError), "forward proxy failed to establish the CONNECT tunnel");
  EXPECT_EQ(ErrcToStr(HttpClientErrc::ioError), "internal I/O error");
}

// The "unknown" fallback guards against an out-of-range value (e.g. from a future enum extension or a
// corrupted cast) without throwing.
TEST(HttpClientErrcTest, UnknownCodeFallsBack) {
  EXPECT_EQ(ErrcToStr(static_cast<HttpClientErrc>(0xFF)), "unknown error");
}

namespace {

// Wait until `fd` is readable (or timeoutMs elapses); poll on POSIX, select on Windows.
bool WaitReadable(NativeHandle fd, int timeoutMs) {
#ifdef AERONET_WINDOWS
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(fd, &readSet);
  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  return ::select(0, &readSet, nullptr, nullptr, &tv) > 0;
#else
  pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};  // NOLINT(misc-include-cleaner)
  return ::poll(&pfd, 1, timeoutMs) > 0;                 // NOLINT(misc-include-cleaner)
#endif
}

// Read the request header block up to (and including) the terminating "\r\n\r\n". Returns what was read so
// far on EOF (empty when the peer closed without sending anything). Good enough for bodyless GET / CONNECT.
std::string ReadRequestHead(NativeHandle fd) {
  std::string buf;
  char tmp[1024];
  for (;;) {
    const auto sz = ::recv(fd, tmp, static_cast<int>(sizeof(tmp)), 0);
    if (sz <= 0) {
      return buf;
    }
    buf.append(tmp, static_cast<std::size_t>(sz));
    if (buf.contains("\r\n\r\n")) {
      return buf;
    }
  }
}

void SendAll(NativeHandle fd, std::string_view data) {
  std::size_t off = 0;
  while (off < data.size()) {
    const int64_t sz = SafeSend(fd, data.data() + off, data.size() - off);
    if (sz <= 0) {
      return;
    }
    off += static_cast<std::size_t>(sz);
  }
}

// Blocking connect to 127.0.0.1:port (aeronet servers bind IPv4 loopback). Returns kInvalidHandle on error.
NativeHandle ConnectLoopbackV4(uint16_t port) {
  const NativeHandle fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd == kInvalidHandle) {
    return kInvalidHandle;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseNativeHandle(fd);
    return kInvalidHandle;
  }
  return fd;
}

// Extract the port from a "CONNECT host:port ..." request line (tests only ever tunnel to host:port).
uint16_t ParseConnectPort(std::string_view head) {
  const auto sp1 = head.find(' ');
  const auto sp2 = head.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
    return 0;
  }
  const std::string_view authority = head.substr(sp1 + 1, sp2 - sp1 - 1);
  const auto colon = authority.rfind(':');
  if (colon == std::string_view::npos) {
    return 0;
  }
  uint16_t port = 0;
  for (const char ch : authority.substr(colon + 1)) {
    if (ch < '0' || ch > '9') {
      break;
    }
    port = static_cast<uint16_t>((port * 10) + (ch - '0'));
  }
  return port;
}

// Forward bytes both ways between `a` and `b` until either side closes (or the proxy is stopping).
void PumpTunnel(NativeHandle lhs, NativeHandle rhs, const std::atomic<bool>& stop) {
  char buf[8192];
  while (!stop.load(std::memory_order_relaxed)) {
    const NativeHandle pair[2][2] = {{lhs, rhs}, {rhs, lhs}};
    for (const auto& dir : pair) {
      if (!WaitReadable(dir[0], 10)) {
        continue;
      }
      const auto sz = ::recv(dir[0], buf, static_cast<int>(sizeof(buf)), 0);
      if (sz <= 0) {
        return;  // EOF / error on either half ends the tunnel
      }
      SendAll(dir[1], std::string_view(buf, static_cast<std::size_t>(sz)));
    }
  }
}

// A minimal forward proxy bound to an ephemeral IPv4 loopback port. It terminates absolute-form http
// requests itself (replying with a canned response and capturing the first request head), and for CONNECT
// either refuses (when a reject response is configured) or tunnels raw bytes to the requested loopback port.
class TinyProxy {
 public:
  // rejectResponse: when non-empty, CONNECT requests are answered with it (and closed) instead of tunnelled.
  // dropOnConnect: when true, CONNECT requests are dropped (socket closed with no response at all).
  explicit TinyProxy(std::string httpResponse = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nhey",
                     std::string rejectResponse = {}, bool dropOnConnect = false)
      : _httpResponse(std::move(httpResponse)),
        _rejectResponse(std::move(rejectResponse)),
        _dropOnConnect(dropOnConnect) {
#ifdef AERONET_WINDOWS
    EnsureWinsockInitialized();
#endif
    _listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_NE(_listenFd, kInvalidHandle);
    int one = 1;
    ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    EXPECT_EQ(::bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t len = sizeof(addr);
    EXPECT_EQ(::getsockname(_listenFd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    _port = ntohs(addr.sin_port);
    EXPECT_EQ(::listen(_listenFd, 8), 0);
    _acceptThread = std::thread([this] { acceptLoop(); });
  }

  TinyProxy(const TinyProxy&) = delete;
  TinyProxy& operator=(const TinyProxy&) = delete;
  TinyProxy(TinyProxy&&) = delete;
  TinyProxy& operator=(TinyProxy&&) = delete;

  ~TinyProxy() {
    _stop.store(true, std::memory_order_relaxed);
    if (_acceptThread.joinable()) {
      _acceptThread.join();  // no more workers are spawned after this returns
    }
    for (std::thread& worker : _workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    if (_listenFd != kInvalidHandle) {
      CloseNativeHandle(_listenFd);
    }
  }

  [[nodiscard]] uint16_t port() const noexcept { return _port; }
  [[nodiscard]] int connectCount() const noexcept { return _connectCount.load(std::memory_order_relaxed); }
  [[nodiscard]] std::string capturedHead() {
    const std::scoped_lock lock(_mtx);
    return _capturedHead;
  }

 private:
  void acceptLoop() {
    while (!_stop.load(std::memory_order_relaxed)) {
      if (!WaitReadable(_listenFd, 20)) {
        continue;
      }
      const NativeHandle fd = ::accept(_listenFd, nullptr, nullptr);
      if (fd == kInvalidHandle) {
        continue;
      }
      _workers.emplace_back([this, fd] { handleConnection(fd); });
    }
  }

  void handleConnection(NativeHandle clientFd) {
    const std::string head = ReadRequestHead(clientFd);
    if (head.empty()) {
      CloseNativeHandle(clientFd);
      return;
    }
    if (head.starts_with("CONNECT ")) {
      _connectCount.fetch_add(1, std::memory_order_relaxed);
      if (_dropOnConnect) {
        CloseNativeHandle(clientFd);  // drop the tunnel with no response at all
        return;
      }
      if (!_rejectResponse.empty()) {
        SendAll(clientFd, _rejectResponse);
        CloseNativeHandle(clientFd);
        return;
      }
      const NativeHandle targetFd = ConnectLoopbackV4(ParseConnectPort(head));
      if (targetFd == kInvalidHandle) {
        SendAll(clientFd, "HTTP/1.1 502 Bad Gateway\r\n\r\n");
        CloseNativeHandle(clientFd);
        return;
      }
      SendAll(clientFd, "HTTP/1.1 200 Connection Established\r\n\r\n");
      PumpTunnel(clientFd, targetFd, _stop);
      CloseNativeHandle(targetFd);
      CloseNativeHandle(clientFd);
      return;
    }
    // Absolute-form http request: capture it and answer directly (looping to serve keep-alive requests).
    capture(head);
    SendAll(clientFd, _httpResponse);
    while (!_stop.load(std::memory_order_relaxed)) {
      const std::string next = ReadRequestHead(clientFd);
      if (next.empty()) {
        break;
      }
      capture(next);
      SendAll(clientFd, _httpResponse);
    }
    CloseNativeHandle(clientFd);
  }

  void capture(const std::string& head) {
    const std::scoped_lock lock(_mtx);
    if (_capturedHead.empty()) {
      _capturedHead = head;
    }
  }

  std::string _httpResponse;
  std::string _rejectResponse;
  bool _dropOnConnect;
  std::thread _acceptThread;
  std::vector<std::thread> _workers;
  std::mutex _mtx;
  std::string _capturedHead;
  std::atomic<bool> _stop{false};
  std::atomic<int> _connectCount{0};
  NativeHandle _listenFd{kInvalidHandle};
  uint16_t _port{0};
};

std::string ProxyUrl(uint16_t port) { return "http://127.0.0.1:" + std::to_string(port); }

}  // namespace

// A plain http origin reached through the proxy: the client must send the request in absolute-form
// ("GET http://origin/path HTTP/1.1") so the proxy knows which origin to forward to, while the Host header
// still names the origin. The origin host never has to resolve -- the client connects to the proxy.
TEST(HttpClientProxyE2ETest, HttpOriginUsesAbsoluteFormRequestTarget) {
  TinyProxy proxy("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nproxy");
  HttpClientConfig cfg;
  cfg.withProxy(ProxyUrl(proxy.port()));
  HttpClient client(cfg);

  const auto result = client.get("http://origin.invalid:1234/path?x=1");
  ASSERT_TRUE(result) << ErrcToStr(result.error());
  EXPECT_EQ(result->status(), 200);
  EXPECT_EQ(result->bodyInMemory(), "proxy");

  const std::string head = proxy.capturedHead();
  EXPECT_TRUE(head.starts_with("GET http://origin.invalid:1234/path?x=1 HTTP/1.1\r\n")) << head;
  EXPECT_NE(head.find("Host: origin.invalid:1234\r\n"), std::string::npos) << head;
  EXPECT_EQ(proxy.connectCount(), 0);  // plain http never issues CONNECT
}

// The absolute-form target spells out a default port too ("http://host:80/..."), and keep-alive reuse works
// through the proxy (both requests land on the one pooled connection the proxy terminates).
TEST(HttpClientProxyE2ETest, HttpOriginDefaultPortAndKeepAliveReuse) {
  TinyProxy proxy("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  HttpClientConfig cfg;
  cfg.withProxy(ProxyUrl(proxy.port()));
  HttpClient client(cfg);

  const auto r1 = client.get("http://example.test/a");
  ASSERT_TRUE(r1) << ErrcToStr(r1.error());
  EXPECT_EQ(r1->status(), 200);
  const auto r2 = client.get("http://example.test/b");
  ASSERT_TRUE(r2) << ErrcToStr(r2.error());
  EXPECT_EQ(r2->status(), 200);

  EXPECT_TRUE(proxy.capturedHead().starts_with("GET http://example.test:80/a HTTP/1.1\r\n")) << proxy.capturedHead();
}

#ifdef AERONET_ENABLE_OPENSSL

namespace {

// HTTPS origin reached through the proxy: the client opens a CONNECT tunnel to the origin, then completes
// the TLS handshake with the real aeronet TLS server through it. Cert verification is off (self-signed).
class HttpClientProxyTlsE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto [certPem, keyPem] = test::MakeEphemeralCertKey("localhost");
    _certPem = certPem;

    TLSConfig tls;
    tls.enabled = true;
    tls.withCertPem(certPem).withKeyPem(keyPem);

    Router router;
    router.setPath(http::Method::GET, "/secure",
                   [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, "secret", "text/plain"); });

    HttpServerConfig scfg;
    scfg.withPort(0).withKeepAliveTimeout(std::chrono::seconds{5}).withPollInterval(std::chrono::milliseconds{20});
    scfg.tls = std::move(tls);

    _server = std::make_unique<SingleHttpServer>(std::move(scfg), std::move(router));
    _port = _server->port();
    _server->start();
  }

  void TearDown() override { _server.reset(); }

  [[nodiscard]] std::string secureUrl() const { return "https://localhost:" + std::to_string(_port) + "/secure"; }

  std::unique_ptr<SingleHttpServer> _server;
  std::string _certPem;
  uint16_t _port{0};
};
}  // namespace

TEST_F(HttpClientProxyTlsE2ETest, HttpsOriginViaConnectTunnel) {
  TinyProxy proxy;
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;  // self-signed origin cert, not in any trust store
  cfg.withProxy(ProxyUrl(proxy.port()));
  HttpClient client(cfg);

  const auto result = client.get(secureUrl());
  ASSERT_TRUE(result) << ErrcToStr(result.error());
  EXPECT_EQ(result->status(), 200);
  EXPECT_EQ(result->bodyInMemory(), "secret");
  EXPECT_GE(proxy.connectCount(), 1);  // the tunnel went through a CONNECT
}

// The proxy CA overrides the trust store for the (tunnelled) origin handshake: pointing it at the origin's
// own self-signed cert makes full verification (chain + hostname "localhost") succeed through the tunnel.
TEST_F(HttpClientProxyTlsE2ETest, HttpsOriginVerifiedAgainstProxyCa) {
  const std::filesystem::path caPath =
      std::filesystem::temp_directory_path() / ("aeronet-proxy-ca-" + std::to_string(::getpid()) + ".pem");
  std::ofstream(caPath) << _certPem;

  TinyProxy proxy;
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = true;  // verify the origin -- against the proxy CA below
  cfg.withProxy(ProxyUrl(proxy.port()), caPath.string());
  HttpClient client(cfg);

  const auto result = client.get(secureUrl());
  ASSERT_TRUE(result) << ErrcToStr(result.error());
  EXPECT_EQ(result->status(), 200);
  EXPECT_EQ(result->bodyInMemory(), "secret");
  std::filesystem::remove(caPath);
}

// A proxy that refuses the CONNECT (403) surfaces as HttpClientErrc::proxyError -- the tunnel never opens,
// so the TLS handshake is never attempted.
TEST_F(HttpClientProxyTlsE2ETest, ProxyRefusesConnectReturnsProxyError) {
  TinyProxy proxy("", "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  cfg.withProxy(ProxyUrl(proxy.port()));
  HttpClient client(cfg);

  const auto result = client.get(secureUrl());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::proxyError);
  EXPECT_GE(proxy.connectCount(), 1);
}

// A proxy that drops the connection without answering the CONNECT (no bytes at all) also surfaces as
// proxyError -- the client reaches EOF before an end-of-headers marker.
TEST_F(HttpClientProxyTlsE2ETest, ProxyDropsConnectReturnsProxyError) {
  TinyProxy proxy("", "", /*dropOnConnect=*/true);
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  cfg.withProxy(ProxyUrl(proxy.port()));
  HttpClient client(cfg);

  const auto result = client.get(secureUrl());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::proxyError);
}

// A proxy that returns a malformed status line (no status code to parse) is rejected as proxyError.
TEST_F(HttpClientProxyTlsE2ETest, ProxyMalformedStatusLineReturnsProxyError) {
  TinyProxy proxy("", "GARBAGE\r\n\r\n");
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  cfg.withProxy(ProxyUrl(proxy.port()));
  HttpClient client(cfg);

  const auto result = client.get(secureUrl());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::proxyError);
}

#endif  // AERONET_ENABLE_OPENSSL

namespace {

// Read until the end of the request header block ("\r\n\r\n"). The tests only issue bodyless GETs, so
// this is enough to know the client finished writing its request before we script the response.
void DrainRequest(NativeHandle fd) {
  std::string buf;
  char tmp[1024];
  for (;;) {
    const auto sz = ::recv(fd, tmp, static_cast<int>(sizeof(tmp)), 0);
    if (sz <= 0) {
      return;
    }
    buf.append(tmp, static_cast<std::size_t>(sz));
    if (buf.contains("\r\n\r\n")) {
      return;
    }
  }
}

// Minimal scriptable raw TCP server bound to an ephemeral loopback port. For each accepted connection
// it invokes a user handler with the socket fd and a 0-based connection index, then closes the socket.
class RawServer {
 public:
  using Handler = std::function<void(NativeHandle fd, int connIndex)>;

  // family is AF_INET (default) or AF_INET6. The IPv6 path fails softly (listening() stays false) so a
  // test can GTEST_SKIP() on hosts/CI without IPv6 loopback instead of hard-failing.
  explicit RawServer(Handler handler, int family = AF_INET) : _handler(std::move(handler)) {
#ifdef AERONET_WINDOWS
    EnsureWinsockInitialized();
#endif
    if (family == AF_INET6) {
      setupIpv6();
    } else {
      setupIpv4();
    }
    if (_listenFd != kInvalidHandle) {
      _thread = std::thread([this] { run(); });
    }
  }

  RawServer(const RawServer&) = delete;
  RawServer& operator=(const RawServer&) = delete;
  RawServer(RawServer&&) = delete;
  RawServer& operator=(RawServer&&) = delete;

  ~RawServer() {
    _stop.store(true, std::memory_order_relaxed);
    if (_thread.joinable()) {
      _thread.join();
    }
    if (_listenFd != kInvalidHandle) {
      CloseNativeHandle(_listenFd);
    }
  }

  [[nodiscard]] uint16_t port() const noexcept { return _port; }
  [[nodiscard]] bool listening() const noexcept { return _listenFd != kInvalidHandle; }

 private:
  void setupIpv4() {
    _listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_NE(_listenFd, kInvalidHandle);
    int one = 1;
    // NOLINTNEXTLINE(misc-include-cleaner)
    ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    EXPECT_EQ(::bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t len = sizeof(addr);
    EXPECT_EQ(::getsockname(_listenFd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    _port = ntohs(addr.sin_port);
    EXPECT_EQ(::listen(_listenFd, 8), 0);
  }

  // Soft setup on IPv6 loopback: any failure leaves _listenFd == kInvalidHandle so the test skips.
  void setupIpv6() {
    _listenFd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (_listenFd == kInvalidHandle) {
      return;
    }
    int one = 1;
    // NOLINTNEXTLINE(misc-include-cleaner)
    ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_loopback;
    addr.sin6_port = 0;
    if (::bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      CloseNativeHandle(_listenFd);
      _listenFd = kInvalidHandle;
      return;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(_listenFd, reinterpret_cast<sockaddr*>(&addr), &len) != 0 || ::listen(_listenFd, 8) != 0) {
      CloseNativeHandle(_listenFd);
      _listenFd = kInvalidHandle;
      return;
    }
    _port = ntohs(addr.sin6_port);
  }

  void run() {
    int idx = 0;
    while (!_stop.load(std::memory_order_relaxed)) {
      if (!WaitReadable(_listenFd, 20)) {
        continue;  // timeout (re-check the stop flag) or interrupted
      }
      const NativeHandle fd = ::accept(_listenFd, nullptr, nullptr);
      if (fd == kInvalidHandle) {
        continue;
      }
      _handler(fd, idx++);
      CloseNativeHandle(fd);
    }
  }

  Handler _handler;
  std::thread _thread;
  std::atomic<bool> _stop{false};
  NativeHandle _listenFd{kInvalidHandle};
  uint16_t _port{0};
};

std::string MakeUrl(uint16_t port, std::string_view path = "/") {
  return "http://127.0.0.1:" + std::to_string(port) + std::string(path);
}

}  // namespace

// A hard DNS resolution failure: .invalid is reserved (RFC 6761) and never resolves, so ConnectTCP
// reports an immediate failure. Exercises connectNew()'s failure branch and the subsequent
// event-loop registration failure on the resulting empty connection.
TEST(HttpClientErrorE2ETest, DnsResolutionFailureReturnsError) {
  HttpClient client;
  auto result = client.get("http://aeronet-nonexistent-host.invalid/");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::connectFailed);
}

// The server promises a 100-byte body but sends 5 bytes then closes: the client hits EOF before the
// body is complete. The parser cannot tell a truncated body from a malformed one (both are an
// unparseable response once no more bytes will arrive), so the truncation surfaces as malformedResponse.
// With retries disabled it is not retried away.
TEST(HttpClientErrorE2ETest, ClosedBeforeCompleteResponseReturnsError) {
  RawServer server([](NativeHandle fd, int) {
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
  });
  HttpClientConfig cfg;  // default retry policy: a fresh-connection post-send failure is never retried
  HttpClient client(cfg);
  auto result = client.get(MakeUrl(server.port()));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::malformedResponse);
}

// The server reads the request but never answers; the client must give up at its request deadline.
TEST(HttpClientErrorE2ETest, ReadTimeoutReturnsError) {
  RawServer server([](NativeHandle fd, int) {
    DrainRequest(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds{200});  // outlive the client's deadline
  });
  HttpClientConfig cfg;
  cfg.requestTimeout = std::chrono::milliseconds{150};
  HttpClient client(cfg);
  auto result = client.get(MakeUrl(server.port()));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::timeout);
}

// First request pools a keep-alive connection that the server then closes; the second request finds the
// pooled connection dead and transparently retries on a fresh one (the stale keep-alive race).
TEST(HttpClientErrorE2ETest, StaleKeepAliveConnectionRetriedOnFreshSucceeds) {
  RawServer server([](NativeHandle fd, int idx) {
    DrainRequest(fd);
    SendAll(fd, idx == 0 ? "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst"
                         : "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecond");
  });
  HttpClientConfig cfg;
  cfg.requestTimeout = std::chrono::seconds{2};  // safety net so a worst-case stale read cannot hang
  HttpClient client(cfg);

  auto r1 = client.get(MakeUrl(server.port())).value();
  EXPECT_EQ(r1.status(), 200);
  EXPECT_EQ(r1.bodyInMemory(), "first");

  // Give the server's close of connection #0 time to reach the client before we reuse it.
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  auto r2 = client.get(MakeUrl(server.port())).value();
  EXPECT_EQ(r2.status(), 200);
  EXPECT_EQ(r2.bodyInMemory(), "second");
}

// First request pools a keep-alive connection; the server is then torn down entirely. The next request
// finds the pooled connection dead, retries on a fresh connect that is refused, and on the retry path
// drops the now-useless pooled bucket before the final (also failing) attempt.
TEST(HttpClientErrorE2ETest, RetryExhaustionDropsDeadPoolThenReturnsError) {
  auto server = std::make_unique<RawServer>([](NativeHandle fd, int) {
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst");
  });
  const uint16_t port = server->port();

  HttpClientConfig cfg;
  cfg.connectTimeout = std::chrono::milliseconds{300};
  cfg.requestTimeout = std::chrono::seconds{2};
  HttpClient client(cfg);

  auto r1 = client.get(MakeUrl(port)).value();
  EXPECT_EQ(r1.bodyInMemory(), "first");

  server.reset();  // dead pooled connection AND refused fresh connects
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  auto result = client.get(MakeUrl(port));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::connectFailed);
}

// A request whose bytes were fully written must NOT be retried when the response then fails (here: a
// truncated body), even with the default retry budget - otherwise a non-idempotent POST would be silently
// re-submitted. The server must therefore observe exactly one connection.
TEST(HttpClientErrorE2ETest, RequestNotReSentAfterBytesWritten) {
  std::atomic<int> connections{0};
  RawServer server([&](NativeHandle fd, int) {
    connections.fetch_add(1, std::memory_order_relaxed);
    DrainRequest(fd);
    // Promise 100 body bytes but deliver 5 then close: the failure surfaces only after the full request
    // was written.
    SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
  });
  HttpClient client;  // default retry policy: a sent request is never re-submitted
  auto result = client.post(MakeUrl(server.port()), "payload");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::malformedResponse);
  // Leave time for a (wrongful) retry connection to be accepted before asserting.
  std::this_thread::sleep_for(std::chrono::milliseconds{80});
  EXPECT_EQ(connections.load(std::memory_order_relaxed), 1);
}

// The stale keep-alive race must still be handled for non-idempotent methods: a POST that finds its pooled
// connection already closed is transparently sent on a fresh connection (and exactly once).
TEST(HttpClientErrorE2ETest, PostOnStalePooledConnectionUsesFreshConnection) {
  RawServer server([](NativeHandle fd, int idx) {
    DrainRequest(fd);
    SendAll(fd, idx == 0 ? "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
                         : "HTTP/1.1 201 Created\r\nContent-Length: 4\r\n\r\ndone");
  });
  HttpClient client;  // keep-alive on; the free pre-send stale-pool retry applies even to POST
  EXPECT_EQ(client.post(MakeUrl(server.port()), "a").value().status(), 200);  // pools the connection
  std::this_thread::sleep_for(std::chrono::milliseconds{60});                 // let the server close conn #0
  const HttpResponse r2 = client.post(MakeUrl(server.port()), "b").value();   // stale pool -> fresh connection
  EXPECT_EQ(r2.status(), 201);
  EXPECT_EQ(r2.bodyInMemory(), "done");
}

namespace {
// A small backoff config for retry tests: a couple of attempts with a near-zero delay so the tests stay
// fast while still exercising the sleep path (a 1ms sleep is still a real sleep_for call).
RetryConfig FastRetry(uint32_t maxAttempts) {
  RetryConfig retry;
  retry.maxAttempts = maxAttempts;
  retry.baseDelay = std::chrono::milliseconds{1};
  retry.maxDelay = std::chrono::milliseconds{4};
  return retry;
}
}  // namespace

// A retryable status (503) recovered on the *same* kept-alive connection: the server answers 503 then 200
// over one connection, so the client retries by reusing the pooled connection (it is never stale here).
TEST(HttpClientErrorE2ETest, StatusRetryReusesKeepAliveConnection) {
  std::atomic<int> connections{0};
  RawServer server([&](NativeHandle fd, int) {
    connections.fetch_add(1, std::memory_order_relaxed);
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n");
    DrainRequest(fd);  // the retry arrives on the same kept-alive connection
    SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  });
  HttpClientConfig cfg;
  cfg.withRetry(FastRetry(2));  // 429 / 503 are retried by default
  HttpClient client(cfg);

  const auto result = client.get(MakeUrl(server.port()));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->status(), 200);
  EXPECT_EQ(result->bodyInMemory(), "ok");
  EXPECT_EQ(connections.load(std::memory_order_relaxed), 1);  // reused the one connection
}

// A retry budget that runs out hands back the last (retryable) response in the success state -- a 503 is a
// normal HttpResponse, not an HttpClientErrc. `Connection: close` forces each retry onto a fresh connection.
TEST(HttpClientErrorE2ETest, StatusRetryExhaustedReturnsLastResponse) {
  std::atomic<int> connections{0};
  RawServer server([&](NativeHandle fd, int) {
    connections.fetch_add(1, std::memory_order_relaxed);
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  });
  HttpClientConfig cfg;
  cfg.withRetry(FastRetry(2));
  HttpClient client(cfg);

  const auto result = client.get(MakeUrl(server.port()));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->status(), 503);
  EXPECT_EQ(connections.load(std::memory_order_relaxed), 2);  // initial attempt + one retry
}

// A status outside the retry set (500 is not in the default {429, 503}) is returned immediately, untouched.
TEST(HttpClientErrorE2ETest, StatusNotInRetrySetIsNotRetried) {
  std::atomic<int> connections{0};
  RawServer server([&](NativeHandle fd, int) {
    connections.fetch_add(1, std::memory_order_relaxed);
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  });
  HttpClientConfig cfg;
  cfg.withRetry(FastRetry(3));
  HttpClient client(cfg);

  const auto result = client.get(MakeUrl(server.port()));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->status(), 500);
  EXPECT_EQ(connections.load(std::memory_order_relaxed), 1);
}

// A delta-seconds Retry-After is honored (and capped at maxDelay): with maxDelay = 10ms a "Retry-After: 5"
// is clamped to (effectively) no wait, so the retry still happens promptly and recovers on the same conn.
TEST(HttpClientErrorE2ETest, StatusRetryHonorsRetryAfter) {
  for (std::string_view retryAfterValue : {"5", "5invalid"}) {
    RawServer server([retryAfterValue](NativeHandle fd, int) {
      DrainRequest(fd);
      RawChars data;
      data.append("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nRetry-After: ");
      data.append(retryAfterValue);
      data.append("\r\n\r\n");
      SendAll(fd, data);
      DrainRequest(fd);
      SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone");
    });
    RetryConfig retry = FastRetry(2);
    retry.maxDelay = std::chrono::milliseconds{10};  // Retry-After: 5s is clamped to this
    HttpClientConfig cfg;
    cfg.withRetry(retry);
    HttpClient client(cfg);

    const auto start = std::chrono::steady_clock::now();
    const auto result = client.get(MakeUrl(server.port()));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status(), 200);
    EXPECT_EQ(result->bodyInMemory(), "done");
    // The 5-second Retry-After was capped to maxDelay (10ms): the whole exchange must finish promptly, not
    // after a literal 5s sleep.
    EXPECT_LT(elapsed, std::chrono::seconds{2});
  }
}

// A retryable status with keep-alive disabled and Retry-After honoring turned off: the client drops the
// (non-poolable) connection between attempts and ignores the server's Retry-After, using its own computed
// backoff instead. Two fresh connections; the long Retry-After never delays the retry.
TEST(HttpClientErrorE2ETest, StatusRetryWithoutKeepAliveIgnoresRetryAfter) {
  std::atomic<int> connections{0};
  RawServer server([&](NativeHandle fd, int idx) {
    connections.fetch_add(1, std::memory_order_relaxed);
    DrainRequest(fd);
    SendAll(fd, idx == 0 ? "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nRetry-After: 9\r\n\r\n"
                         : "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  });
  RetryConfig retry = FastRetry(2);
  retry.honorRetryAfter = false;  // ignore the server's Retry-After header; use computed backoff
  HttpClientConfig cfg;
  cfg.keepAlive = false;  // no pooling: each attempt runs on (and drops) a fresh connection
  cfg.withRetry(retry);
  HttpClient client(cfg);

  const auto start = std::chrono::steady_clock::now();
  const auto result = client.get(MakeUrl(server.port()));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(result);
  EXPECT_EQ(result->status(), 200);
  EXPECT_EQ(result->bodyInMemory(), "ok");
  EXPECT_EQ(connections.load(std::memory_order_relaxed), 2);  // two fresh connections, no reuse
  EXPECT_LT(elapsed, std::chrono::seconds{1});                // Retry-After: 9s was ignored, not honored
}

// A post-send failure on an *idempotent* method (GET) is retried only when the caller opts in: the first
// connection truncates its response, the second answers fully.
TEST(HttpClientErrorE2ETest, IdempotentPostSendFailureRetriedWhenEnabled) {
  std::atomic<int> connections{0};
  RawServer server([&](NativeHandle fd, int idx) {
    connections.fetch_add(1, std::memory_order_relaxed);
    DrainRequest(fd);
    SendAll(fd, idx == 0 ? "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort"  // truncated -> malformed
                         : "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
  });
  RetryConfig retry = FastRetry(2);
  retry.retryIdempotentAfterSend = true;
  HttpClientConfig cfg;
  cfg.withRetry(retry);
  HttpClient client(cfg);

  const auto result = client.get(MakeUrl(server.port()));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->status(), 200);
  EXPECT_EQ(result->bodyInMemory(), "hello");
  EXPECT_EQ(connections.load(std::memory_order_relaxed), 2);
}

// A non-idempotent method (POST) is never re-submitted after its bytes were written, even with
// retryIdempotentAfterSend enabled: the truncated response surfaces as an error and the server sees one conn.
TEST(HttpClientErrorE2ETest, NonIdempotentPostSendFailureNotRetried) {
  std::atomic<int> connections{0};
  RawServer server([&](NativeHandle fd, int) {
    connections.fetch_add(1, std::memory_order_relaxed);
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
  });
  RetryConfig retry = FastRetry(3);
  retry.retryIdempotentAfterSend = true;  // still excludes POST (not idempotent)
  HttpClientConfig cfg;
  cfg.withRetry(retry);
  HttpClient client(cfg);

  const auto result = client.post(MakeUrl(server.port()), "payload");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::malformedResponse);
  std::this_thread::sleep_for(std::chrono::milliseconds{80});
  EXPECT_EQ(connections.load(std::memory_order_relaxed), 1);
}

// A connect failure is a pre-send failure, so it is retried (with backoff) up to the attempt budget before
// surfacing. Jitter is enabled to drive the backoff PRNG. The origin is a torn-down server (refused connects).
TEST(HttpClientErrorE2ETest, ConnectFailureRetriedThenExhausted) {
  auto server = std::make_unique<RawServer>([](NativeHandle fd, int) { DrainRequest(fd); });
  const uint16_t port = server->port();
  server.reset();  // nothing listens on `port` now: every connect is refused
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  RetryConfig retry = FastRetry(3);
  retry.jitter = 0.5F;  // exercise the jitter PRNG path
  HttpClientConfig cfg;
  cfg.connectTimeout = std::chrono::milliseconds{200};
  cfg.withRetry(retry);
  HttpClient client(cfg);

  const auto result = client.get(MakeUrl(port));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::connectFailed);
}

// A maxAttempts of 0 is treated as 1 (a single attempt): a normal request still succeeds.
TEST(HttpClientErrorE2ETest, ZeroMaxAttemptsTreatedAsSingleAttempt) {
  RawServer server([](NativeHandle fd, int) {
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
  });
  RetryConfig retry;
  retry.maxAttempts = 0;
  HttpClientConfig cfg;
  cfg.withRetry(retry);
  HttpClient client(cfg);

  const auto result = client.get(MakeUrl(server.port()));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->status(), 200);
}

// An IPv6 literal authority must be bracketed in the Host header: "Host: [::1]:<port>", never "::1:<port>".
TEST(HttpClientErrorE2ETest, Ipv6LiteralHostHeaderIsBracketed) {
  std::mutex mtx;
  std::string capturedHead;
  RawServer server(
      [&](NativeHandle fd, int) {
        std::string head = ReadRequestHead(fd);
        {
          const std::scoped_lock lock(mtx);
          capturedHead = std::move(head);
        }
        SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
      },
      AF_INET6);
  if (!server.listening()) {
    GTEST_SKIP() << "no IPv6 loopback available";
  }
  HttpClient client;
  const std::string url = "http://[::1]:" + std::to_string(server.port()) + "/";
  EXPECT_EQ(client.get(url).value().status(), 200);

  const std::scoped_lock lock(mtx);
  const std::string expected = "Host: [::1]:" + std::to_string(server.port()) + "\r\n";
  EXPECT_NE(capturedHead.find(expected), std::string::npos) << "request head was:\n" << capturedHead;
}

#ifdef AERONET_ENABLE_HTTP2

// HTTP/2 (prior-knowledge h2c) error paths against the scriptable raw server. The client sends its
// connection preface + SETTINGS + HEADERS in one flight; the preface magic ends with "\r\n\r\n" so
// DrainRequest() still delimits "the client has written its request".

namespace {
HttpClientConfig Http2RawConfig() {
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2);
  return cfg;
}
}  // namespace

// The server closes right after the request reaches it: EOF before the stream completed.
TEST(HttpClientErrorE2ETest, Http2AbruptCloseReturnsConnectionClosed) {
  RawServer server([](NativeHandle fd, int) { DrainRequest(fd); });
  HttpClient client(Http2RawConfig());
  auto result = client.get(MakeUrl(server.port()));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::connectionClosed);
}

// The server answers an HTTP/2 preface with an HTTP/1.1 response: the bytes parse as an oversized
// frame, a connection-level protocol error surfaced as malformedResponse.
TEST(HttpClientErrorE2ETest, Http2NonHttp2PeerReturnsMalformedResponse) {
  RawServer server([](NativeHandle fd, int) {
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
  });
  HttpClient client(Http2RawConfig());
  auto result = client.get(MakeUrl(server.port()));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::malformedResponse);
}

// The server reads the request but never answers; the client must give up at its request deadline.
TEST(HttpClientErrorE2ETest, Http2ReadTimeoutReturnsError) {
  RawServer server([](NativeHandle fd, int) {
    DrainRequest(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds{200});  // outlive the client's deadline
  });
  HttpClientConfig cfg = Http2RawConfig();
  cfg.requestTimeout = std::chrono::milliseconds{150};
  HttpClient client(cfg);
  auto result = client.get(MakeUrl(server.port()));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::timeout);
}

#endif  // AERONET_ENABLE_HTTP2

TEST(HttpClientConfigTest, DefaultsAreSane) {
  HttpClientConfig cfg;
  EXPECT_EQ(cfg.userAgent(), "aeronet-client");
  EXPECT_TRUE(cfg.defaultAcceptEncoding().empty());
  EXPECT_TRUE(cfg.followRedirects);
  EXPECT_TRUE(cfg.keepAlive);
  EXPECT_EQ(cfg.tcpNoDelay, TcpNoDelayMode::Auto);
  EXPECT_FALSE(cfg.requestCompression.enabled());
  EXPECT_EQ(cfg.requestCompression.encoding, Encoding::none);  // none == disabled (single source of truth)
  // decompression auto-enables whenever at least one decoder is compiled in.
  EXPECT_EQ(cfg.decompression.enable,
            IsEncodingEnabled(Encoding::zstd) || IsEncodingEnabled(Encoding::br) || IsEncodingEnabled(Encoding::gzip));
  EXPECT_EQ(cfg.maxCapturedRequestBodyBytes, 8UL * 1024UL);
  // Default retry policy keeps the historical behaviour: only the free pre-send stale-pool retry.
  EXPECT_EQ(cfg.retry.maxAttempts, 1U);
}

TEST(HttpClientConfigTest, WithRetryBuilder) {
  RetryConfig retry;
  retry.maxAttempts = 4;
  retry.retryIdempotentAfterSend = true;
  HttpClientConfig cfg;
  cfg.withRetry(retry);
  EXPECT_EQ(cfg.retry.maxAttempts, 4U);
  EXPECT_TRUE(cfg.retry.retryIdempotentAfterSend);
}

TEST(HttpClientConfigTest, FluentSettersAreChainable) {
  HttpClientConfig cfg;
  cfg.withUserAgent("my-agent/2.0")
      .withDefaultAcceptEncoding("gzip")
      .withTcpNoDelayMode(TcpNoDelayMode::Disabled)
      .withKeepAliveTimeout(std::chrono::seconds{5})
      .withMaxCapturedRequestBodyBytes(2048);
  EXPECT_EQ(cfg.userAgent(), "my-agent/2.0");
  EXPECT_EQ(cfg.defaultAcceptEncoding(), "gzip");
  EXPECT_EQ(cfg.tcpNoDelay, TcpNoDelayMode::Disabled);
  EXPECT_EQ(cfg.keepAliveTimeout, std::chrono::seconds{5});
  EXPECT_EQ(cfg.maxCapturedRequestBodyBytes, 2048U);
}

TEST(HttpClientConfigTest, TcpNoDelayBoolHelper) {
  HttpClientConfig cfg;
  cfg.withTcpNoDelay(false);
  EXPECT_EQ(cfg.tcpNoDelay, TcpNoDelayMode::Disabled);
  cfg.withTcpNoDelay();
  EXPECT_EQ(cfg.tcpNoDelay, TcpNoDelayMode::Enabled);
}

TEST(HttpClientConfigTest, DecompressionHelper) {
  HttpClientConfig cfg;
  cfg.withDecompression(false);
  EXPECT_FALSE(cfg.decompression.enable);
  cfg.withDecompression();
  EXPECT_TRUE(cfg.decompression.enable);
}

TEST(HttpClientConfigTest, RequestCompressionHelpers) {
  HttpClientConfig cfg;
  cfg.withRequestCompression(true);  // enable with the default compiled-in codec
  EXPECT_TRUE(cfg.requestCompression.enabled());
  EXPECT_EQ(cfg.requestCompression.encoding, internal::DefaultRequestEncoding());

  cfg.withRequestCompression(Encoding::gzip);  // selects a specific codec
  EXPECT_TRUE(cfg.requestCompression.enabled());
  EXPECT_EQ(cfg.requestCompression.encoding, Encoding::gzip);

  cfg.withRequestCompression(false);  // disabling clears the codec back to none
  EXPECT_FALSE(cfg.requestCompression.enabled());
  EXPECT_EQ(cfg.requestCompression.encoding, Encoding::none);
}

TEST(HttpClientConfigTest, DefaultRequestEncodingMatchesBuild) {
  // The default request codec is the first compiled-in coding in aeronet's preference order.
  Encoding expected = Encoding::none;
  if (IsEncodingEnabled(Encoding::zstd)) {
    expected = Encoding::zstd;
  } else if (IsEncodingEnabled(Encoding::br)) {
    expected = Encoding::br;
  } else if (IsEncodingEnabled(Encoding::gzip)) {
    expected = Encoding::gzip;
  }
  EXPECT_EQ(internal::DefaultRequestEncoding(), expected);
  // Request compression is opt-in: the unset encoding is none, and enabling picks the default codec.
  EXPECT_EQ(HttpClientConfig{}.requestCompression.encoding, Encoding::none);
  EXPECT_EQ(HttpClientConfig{}.withRequestCompression().requestCompression.encoding, expected);
}

TEST(HttpClientConfigTest, ConstructionValidatesCodecConfig) {
  // Valid default config constructs fine and exposes its config back.
  HttpClient ok;
  EXPECT_EQ(ok.config().userAgent(), "aeronet-client");
  ok.clearIdleConnections();  // no-op on a fresh client, but exercises the accessor

  // Encoding::none simply means "request compression disabled": it constructs fine (no longer an error).
  {
    HttpClientConfig cfg;
    cfg.requestCompression.encoding = Encoding::none;
    EXPECT_NO_THROW(HttpClient{cfg});
  }

  // An out-of-range codec parameter is rejected via CompressionConfig::validate() once a codec is set.
  {
    HttpClientConfig cfg;
    cfg.withRequestCompression(Encoding::gzip);
    cfg.requestCompression.codec.maxCompressRatio = 2.0F;  // must be in (0, 1)
    EXPECT_THROW(HttpClient{cfg}, std::exception);
  }
}

TEST(HttpClientConfigTest, Validate) {
  HttpClientConfig cfg;
  EXPECT_NO_THROW(cfg.validate());
  cfg.connectTimeout = std::chrono::milliseconds{0};
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
  cfg.connectTimeout = std::chrono::milliseconds{std::numeric_limits<int>::max() + 1ULL};
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
  cfg.connectTimeout = std::chrono::milliseconds{1};
  EXPECT_NO_THROW(cfg.validate());
}

#ifdef AERONET_ENABLE_OPENSSL
TEST(HttpClientConfigTest, TlsSettersAndGetters) {
  HttpClientConfig cfg;
  cfg.withTlsCaFile("/tmp/ca.pem")
      .withTlsCaPath("/tmp/ca")
      .withTlsCipherList("HIGH")
      .withTlsClientCertKeyFile("/tmp/cert.pem", "/tmp/key.pem")
      .withTlsMinVersion(TLSConfig::TLS_1_2)
      .withTlsMaxVersion(TLSConfig::TLS_1_3);
  EXPECT_EQ(cfg.tlsCaFile(), "/tmp/ca.pem");
  EXPECT_STREQ(cfg.tlsCaFileCStr(), "/tmp/ca.pem");
  EXPECT_EQ(cfg.tlsCaPath(), "/tmp/ca");
  EXPECT_STREQ(cfg.tlsCaPathCStr(), "/tmp/ca");
  EXPECT_EQ(cfg.tlsCipherList(), "HIGH");
  EXPECT_STREQ(cfg.tlsCipherListCStr(), "HIGH");
  EXPECT_EQ(cfg.tlsClientCertFile(), "/tmp/cert.pem");
  EXPECT_STREQ(cfg.tlsClientCertFileCStr(), "/tmp/cert.pem");
  EXPECT_EQ(cfg.tlsClientKeyFile(), "/tmp/key.pem");
  EXPECT_STREQ(cfg.tlsClientKeyFileCStr(), "/tmp/key.pem");
  EXPECT_EQ(cfg.tlsMinVersion, TLSConfig::TLS_1_2);
  EXPECT_EQ(cfg.tlsMaxVersion, TLSConfig::TLS_1_3);
}

TEST(HttpClientConfigTest, TlsInMemoryClientCert) {
  HttpClientConfig cfg;
  cfg.withTlsClientCertKeyMemory("CERT-PEM", "KEY-PEM");
  EXPECT_EQ(cfg.tlsClientCertPem(), "CERT-PEM");
  EXPECT_EQ(cfg.tlsClientKeyPem(), "KEY-PEM");
}
#endif

TEST(HttpClientConfigTest, HttpVersionDefaultsToAuto) {
  HttpClientConfig cfg;
  EXPECT_EQ(cfg.httpVersion, HttpVersionMode::Auto);
}

TEST(HttpClientConfigTest, WithHttpVersionAndHttp2ConfigBuilders) {
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2).withHttp2Config(Http2Config{}.withMaxConcurrentStreams(7));
  EXPECT_EQ(cfg.httpVersion, HttpVersionMode::Http2);
  EXPECT_EQ(cfg.http2.maxConcurrentStreams, 7U);
}

TEST(HttpClientConfigTest, ValidateChecksHttp2SettingsWhenHttp2IsPossible) {
  HttpClientConfig cfg;
  cfg.http2.maxFrameSize = 1;  // below the RFC 9113 minimum
  // HTTP/1.1-only clients never consult the HTTP/2 settings, so validation ignores them.
  cfg.withHttpVersion(HttpVersionMode::Http1_1);
  EXPECT_NO_THROW(cfg.validate());
#ifdef AERONET_ENABLE_HTTP2
  cfg.withHttpVersion(HttpVersionMode::Auto);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
  cfg.withHttpVersion(HttpVersionMode::Http2);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
#else
  // Requiring HTTP/2 in a build without the engine is rejected outright.
  cfg.withHttpVersion(HttpVersionMode::Http2);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
#endif
}

TEST(HttpClientConfigTest, ValidateRejectsHttp2Push) {
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http2).withHttp2Config(Http2Config{}.withEnablePush(true));
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, ProxyDefaultsToDisabled) {
  HttpClientConfig cfg;
  EXPECT_FALSE(cfg.hasProxy());
  EXPECT_TRUE(cfg.proxyUrl().empty());
  EXPECT_TRUE(cfg.proxyCaFile().empty());
}

TEST(HttpClientConfigTest, WithProxyUrlOnly) {
  HttpClientConfig cfg;
  cfg.withProxy("http://127.0.0.1:8080");
  EXPECT_TRUE(cfg.hasProxy());
  EXPECT_EQ(cfg.proxyUrl(), "http://127.0.0.1:8080");
  EXPECT_TRUE(cfg.proxyCaFile().empty());
}

TEST(HttpClientConfigTest, WithProxyUrlAndCaFile) {
  HttpClientConfig cfg;
  cfg.withProxy("http://proxy.local:3128", "/etc/mitmproxy/ca.pem");
  EXPECT_TRUE(cfg.hasProxy());
  EXPECT_EQ(cfg.proxyUrl(), "http://proxy.local:3128");
  EXPECT_EQ(cfg.proxyCaFile(), "/etc/mitmproxy/ca.pem");
  EXPECT_STREQ(cfg.proxyCaFileCStr(), "/etc/mitmproxy/ca.pem");
}

// Constructing an HttpClient parses the proxy endpoint eagerly: a malformed proxy URL or an unsupported
// (https) proxy scheme is a hard setup error surfaced as HttpClientException, not a per-request failure.
TEST(HttpClientConfigTest, InvalidProxyUrlThrowsAtConstruction) {
  HttpClientConfig cfg;
  cfg.withProxy("ftp://proxy.local:3128");  // non-http scheme
  EXPECT_THROW(HttpClient{cfg}, HttpClientException);
}

TEST(HttpClientConfigTest, HttpsProxyRejectedAtConstruction) {
  HttpClientConfig cfg;
  cfg.withProxy("https://proxy.local:3128");  // only cleartext proxies are supported
  EXPECT_THROW(HttpClient{cfg}, HttpClientException);
}

// A bare "host:port" (no scheme) is accepted and assumed to be an http proxy.
TEST(HttpClientConfigTest, SchemelessProxyUrlAccepted) {
  HttpClientConfig cfg;
  cfg.withProxy("127.0.0.1:8080");
  EXPECT_NO_THROW(HttpClient{cfg});
}

TEST(HttpClientConfigTest, CacheDefaultsDisabled) {
  HttpClientConfig cfg;
  EXPECT_FALSE(cfg.cache.enabled());
  EXPECT_EQ(cfg.cache.maxEntries, 1024U);
  EXPECT_EQ(cfg.cache.methods, http::Method::GET | http::Method::HEAD);
}

TEST(HttpClientConfigTest, CacheBuildersSetFields) {
  HttpClientConfig cfg;
  cfg.withCache(std::chrono::seconds{5}).withCacheMaxEntries(32).withCacheMethods(http::Method::GET);
  EXPECT_TRUE(cfg.cache.enabled());
  EXPECT_EQ(cfg.cache.refreshPeriod, std::chrono::seconds{5});
  EXPECT_EQ(cfg.cache.maxEntries, 32U);
  EXPECT_EQ(cfg.cache.methods, static_cast<http::MethodBmp>(http::Method::GET));
}

TEST(HttpClientConfigTest, CacheValidateRejectsUnsafeMethods) {
  HttpClientConfig cfg;
  cfg.withCache(std::chrono::seconds{5}).withCacheMethods(http::Method::GET | http::Method::POST);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, CacheValidateRejectsEmptyMethods) {
  HttpClientConfig cfg;
  cfg.withCache(std::chrono::seconds{5}).withCacheMethods(0);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, CacheValidateRejectsZeroMaxEntries) {
  HttpClientConfig cfg;
  cfg.withCache(std::chrono::seconds{5}).withCacheMaxEntries(0);
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(HttpClientConfigTest, CacheValidationSkippedWhenDisabled) {
  HttpClientConfig cfg;                            // cache disabled by default (zero refresh period)
  cfg.withCacheMaxEntries(0).withCacheMethods(0);  // nonsensical, but ignored while the cache is off
  EXPECT_NO_THROW(cfg.validate());
}

TEST(HttpClientConfigTest, CacheValidConfigAccepted) {
  HttpClientConfig cfg;
  cfg.withCache(std::chrono::seconds{5})
      .withCacheMethods(http::Method::GET | http::Method::HEAD | http::Method::OPTIONS);
  EXPECT_NO_THROW(cfg.validate());
  EXPECT_NO_THROW(HttpClient{cfg});
}

}  // namespace aeronet
