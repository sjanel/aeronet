// End-to-end tests of the cookies of HttpClient (HttpClientConfig::cookies) against aeronet servers, over HTTP/1.1 and
// HTTP/2: storage of the Set-Cookie headers (redirects and retried responses included), the Cookie header sent back,
// merged with the one of the request, SameSite cookies along redirect chains across sites, and the response cache.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/cookie-jar.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/retry-config.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/vector.hpp"
#include "collecting-body-sink.hpp"

namespace aeronet {
namespace {

// The Cookie header received by a server, its cookie-pairs separated by ';' whatever the protocol: the cookies of an
// HTTP/2 request are separate fields, which the server joins with ';'.
std::string ReceivedCookies(const HttpRequestView& req) {
  std::string cookies(req.headerValueOrEmpty(http::Cookie));
  for (std::size_t pos = cookies.find("; "); pos != std::string::npos; pos = cookies.find("; ", pos)) {
    cookies.erase(pos + 1, 1);
  }
  return cookies;
}

// The cookie-pairs of 'cookies' in sorted order, to compare a set of cookies whatever the order of the fields.
std::string Sorted(std::string_view cookies) {
  vector<std::string_view> pairs;
  while (!cookies.empty()) {
    const std::size_t sep = cookies.find(';');
    pairs.push_back(cookies.substr(0, sep));
    cookies = sep == std::string_view::npos ? std::string_view{} : cookies.substr(sep + 1);
  }
  std::ranges::sort(pairs);
  std::string sorted;
  for (const std::string_view pair : pairs) {
    sorted.append(sorted.empty() ? "" : ";").append(pair);
  }
  return sorted;
}

test::TestServer MakeServer() {
  HttpServerConfig cfg;
  cfg.withPort(0).withPollInterval(std::chrono::milliseconds{20});
  return test::TestServer(std::move(cfg));
}

std::atomic<int> gCountedCalls{0};
std::atomic<int> gFlakyCalls{0};

std::string StoreUrl(std::string_view path);
std::string QueueSameSiteUrl(std::string_view path);
std::string QueueCrossSiteUrl(std::string_view path);

HttpResponse Redirect(const HttpRequestView& req, http::StatusCode status, std::string_view location) {
  HttpResponse resp = req.makeResponse(status);
  resp.location(location);
  return resp;
}

// The "store": sets and echoes cookies, and redirects to the "queue" server (another site, reached as localhost) which
// redirects back, like a waiting room in front of a shop.
test::TestServer MakeStore() {
  test::TestServer server = MakeServer();
  auto store = server.router();
  store.setPath(http::Method::GET, "/login", [](const HttpRequestView& req) {
    return req.makeResponse("logged in")
        .headerAddLine(http::SetCookie, "session=abc; Path=/; HttpOnly")
        .headerAddLine(http::SetCookie, "theme=dark");
  });
  store.setPath(http::Method::GET, "/logout", [](const HttpRequestView& req) {
    return req.makeResponse("logged out").headerAddLine(http::SetCookie, "session=; Path=/; Max-Age=0");
  });
  store.setPath(http::Method::GET, "/samesite", [](const HttpRequestView& req) {
    return req.makeResponse("set")
        .headerAddLine(http::SetCookie, "strict=1; SameSite=Strict")
        .headerAddLine(http::SetCookie, "lax=1; SameSite=Lax")
        .headerAddLine(http::SetCookie, "none=1; SameSite=None; Secure")  // the loopback host is a secure context
        .headerAddLine(http::SetCookie, "plain=1");
  });
  store.setPath(http::Method::GET | http::Method::POST, "/echo",
                [](const HttpRequestView& req) { return req.makeResponse(ReceivedCookies(req)); });
  store.setPath(http::Method::GET, "/echo-connection", [](const HttpRequestView& req) {
    return req.makeResponse(ReceivedCookies(req) + '|' + std::string(req.headerValueOrEmpty(http::Connection)));
  });
  store.setPath(http::Method::GET, "/counted", [](const HttpRequestView& req) {
    return req.makeResponse(ReceivedCookies(req) + '|' + std::to_string(++gCountedCalls));
  });
  store.setPath(http::Method::GET, "/redirect-login", [](const HttpRequestView& req) {
    return Redirect(req, http::StatusCodeFound, "/echo").headerAddLine(http::SetCookie, "redirected=1");
  });
  store.setPath(http::Method::GET, "/flaky", [](const HttpRequestView& req) {
    if (++gFlakyCalls == 1) {
      return req.makeResponse(http::StatusCodeServiceUnavailable).headerAddLine(http::SetCookie, "attempt=1");
    }
    return req.makeResponse(ReceivedCookies(req));
  });
  store.setPath(http::Method::GET, "/to-queue", [](const HttpRequestView& req) {
    return Redirect(req, http::StatusCodeFound, QueueCrossSiteUrl("/bounce"));
  });
  store.setPath(http::Method::POST, "/to-queue-keeping-method", [](const HttpRequestView& req) {
    return Redirect(req, http::StatusCodeTemporaryRedirect, QueueCrossSiteUrl("/bounce"));
  });
  store.setPath(http::Method::GET, "/to-same-site", [](const HttpRequestView& req) {
    return Redirect(req, http::StatusCodeFound, QueueSameSiteUrl("/echo"));
  });
  return server;
}

test::TestServer MakeQueue() {
  test::TestServer server = MakeServer();
  auto queue = server.router();
  // The waiting room lets the visitor through at once, with a cookie of its own.
  queue.setPath(http::Method::GET | http::Method::POST, "/bounce", [](const HttpRequestView& req) {
    return Redirect(req, req.method() == http::Method::GET ? http::StatusCodeFound : http::StatusCodeTemporaryRedirect,
                    StoreUrl("/echo"))
        .headerAddLine(http::SetCookie, "queue=1");
  });
  queue.setPath(http::Method::GET, "/echo",
                [](const HttpRequestView& req) { return req.makeResponse(ReceivedCookies(req)); });
  return server;
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
test::TestServer gStore = MakeStore();
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
test::TestServer gQueue = MakeQueue();

std::string StoreUrl(std::string_view path) {
  return "http://127.0.0.1:" + std::to_string(gStore.port()) + std::string(path);
}

// The queue on the same host as the store, another port: another origin of the same site.
std::string QueueSameSiteUrl(std::string_view path) {
  return "http://127.0.0.1:" + std::to_string(gQueue.port()) + std::string(path);
}

// The queue on another host: another site.
std::string QueueCrossSiteUrl(std::string_view path) {
  return "http://localhost:" + std::to_string(gQueue.port()) + std::string(path);
}

class HttpClientCookiesTest : public ::testing::TestWithParam<HttpVersionMode> {
 protected:
  [[nodiscard]] static HttpClientConfig Config() {
    HttpClientConfig cfg;
    cfg.withHttpVersion(GetParam()).withCookies();
    return cfg;
  }

  // The body of the response to GET 'url', which must succeed.
  static std::string Get(HttpClient& client, std::string_view url) {
    const HttpClientResult result = client.get(url);
    if (!result) {
      ADD_FAILURE() << url << ": " << ErrcToStr(result.error());
      return {};
    }
    EXPECT_EQ(result->status(), http::StatusCodeOK) << url;
    return std::string(result->bodyInMemory());
  }

  static std::string Send(HttpClient& client, const HttpRequest& request) {
    const HttpClientResult result = client.request(request);
    if (!result) {
      ADD_FAILURE() << ErrcToStr(result.error());
      return {};
    }
    EXPECT_EQ(result->status(), http::StatusCodeOK);
    return std::string(result->bodyInMemory());
  }
};

TEST_P(HttpClientCookiesTest, CookiesAreOffByDefault) {
  HttpClientConfig cfg;
  cfg.withHttpVersion(GetParam());
  EXPECT_FALSE(cfg.cookies.enabled());
  HttpClient client(cfg);
  Get(client, StoreUrl("/login"));
  EXPECT_TRUE(client.cookieJar().empty());
  EXPECT_EQ(Get(client, StoreUrl("/echo")), "");
}

TEST_P(HttpClientCookiesTest, StoresAndSendsBackCookies) {
  HttpClient client(Config());
  EXPECT_EQ(Get(client, StoreUrl("/echo")), "");
  Get(client, StoreUrl("/login"));
  EXPECT_EQ(client.cookieJar().size(), 2U);
  EXPECT_EQ(Get(client, StoreUrl("/echo")), "session=abc;theme=dark");
  // Cookies ignore the port: the queue on the same host gets them.
  EXPECT_EQ(Get(client, QueueSameSiteUrl("/echo")), "session=abc;theme=dark");
  EXPECT_EQ(Get(client, QueueCrossSiteUrl("/echo")), "");
  Get(client, StoreUrl("/logout"));
  EXPECT_EQ(Get(client, StoreUrl("/echo")), "theme=dark");
}

TEST_P(HttpClientCookiesTest, RedirectResponsesSetCookies) {
  HttpClient client(Config());
  EXPECT_EQ(Get(client, StoreUrl("/redirect-login")), "redirected=1");
}

TEST_P(HttpClientCookiesTest, CookieHeaderOfTheRequestIsKept) {
  HttpClient client(Config());
  Get(client, StoreUrl("/login"));

  HttpRequest request = client.makeRequest(http::Method::GET, StoreUrl("/echo"));
  request.headerAddLine("x-before", "1").headerAddLine(http::Cookie, "user=1");
  EXPECT_EQ(Sorted(Send(client, request)), "session=abc;theme=dark;user=1");

  // The Cookie line is not the last one of the head.
  request = client.makeRequest(http::Method::GET, StoreUrl("/echo"));
  request.headerAddLine(http::Cookie, "user=2").headerAddLine("x-after", "1");
  EXPECT_EQ(Sorted(Send(client, request)), "session=abc;theme=dark;user=2");

  // An empty Cookie header gets the cookies of the jar.
  request = client.makeRequest(http::Method::GET, StoreUrl("/echo"));
  request.headerAddLine(http::Cookie, "");
  EXPECT_EQ(Sorted(Send(client, request)), "session=abc;theme=dark");

  // A Cookie header of the configuration too.
  HttpClientConfig cfg = Config();
  cfg.addGlobalHeader(http::Header(http::Cookie, "global=1"));
  HttpClient globalClient(cfg);
  Get(globalClient, StoreUrl("/login"));
  EXPECT_EQ(Sorted(Get(globalClient, StoreUrl("/echo"))), "global=1;session=abc;theme=dark");
}

// The chain store -> queue (another site) -> store: the SameSite=Strict cookies are withheld when the chain comes back,
// the Lax ones too when the method is not safe, as a browser does for a navigation.
TEST_P(HttpClientCookiesTest, SameSiteCookiesAlongRedirectChains) {
  HttpClient client(Config());
  Get(client, StoreUrl("/samesite"));
  EXPECT_EQ(Get(client, StoreUrl("/echo")), "strict=1;lax=1;none=1;plain=1");

  EXPECT_EQ(Get(client, StoreUrl("/to-queue")), "lax=1;none=1;plain=1");
  // The queue stored its cookie, for its own host.
  EXPECT_EQ(client.cookieJar().cookieHeader(QueueCrossSiteUrl("/")), "queue=1");
  EXPECT_EQ(Get(client, QueueCrossSiteUrl("/echo")), "queue=1");

  HttpRequest post = client.makeRequest(http::Method::POST, StoreUrl("/to-queue-keeping-method"), "form");
  EXPECT_EQ(Send(client, post), "none=1;plain=1");

  // Another port of the same host is the same site.
  EXPECT_EQ(Get(client, StoreUrl("/to-same-site")), "strict=1;lax=1;none=1;plain=1");
}

TEST_P(HttpClientCookiesTest, EachRetrySendsTheCookiesOfTheJar) {
  gFlakyCalls = 0;
  HttpClientConfig cfg = Config();
  RetryConfig retry;
  retry.maxAttempts = 2;
  retry.baseDelay = std::chrono::milliseconds{1};
  cfg.withRetry(retry);
  HttpClient client(cfg);
  EXPECT_EQ(Get(client, StoreUrl("/flaky")), "attempt=1");
  EXPECT_EQ(gFlakyCalls, 2);
}

TEST_P(HttpClientCookiesTest, CachedResponsesDependOnTheCookies) {
  gCountedCalls = 0;
  HttpClientConfig cfg = Config();
  cfg.withCache(std::chrono::hours{1});
  HttpClient client(cfg);
  EXPECT_EQ(Get(client, StoreUrl("/counted")), "|1");
  EXPECT_EQ(Get(client, StoreUrl("/counted")), "|1");
  Get(client, StoreUrl("/login"));
  EXPECT_EQ(Get(client, StoreUrl("/counted")), "session=abc;theme=dark|2");
  EXPECT_EQ(Get(client, StoreUrl("/counted")), "session=abc;theme=dark|2");
}

TEST_P(HttpClientCookiesTest, StreamedResponsesSetCookies) {
  HttpClient client(Config());
  test::CollectingBodySink sink;
  const HttpClientResult result = client.get(StoreUrl("/login"), sink);
  ASSERT_TRUE(result) << ErrcToStr(result.error());
  EXPECT_EQ(sink.body, "logged in");
  EXPECT_EQ(client.cookieJar().size(), 2U);
}

TEST_P(HttpClientCookiesTest, CookiesOfTheJarApi) {
  HttpClient client(Config());
  EXPECT_TRUE(client.cookieJar().setCookie(StoreUrl("/"), "manual=1"));
  // Expired by the time of the next request: not sent, then removed.
  EXPECT_TRUE(
      client.cookieJar().setCookie(StoreUrl("/"), "old=1; Max-Age=1", SysClock::now() - std::chrono::seconds{2}));
  EXPECT_EQ(client.cookieJar().size(), 2U);
  EXPECT_EQ(Get(client, StoreUrl("/echo")), "manual=1");
  client.releaseUnusedMemory();
  EXPECT_EQ(client.cookieJar().size(), 1U);
}

TEST_P(HttpClientCookiesTest, MoveKeepsTheCookiesCopyDoesNot) {
  HttpClient client(Config());
  Get(client, StoreUrl("/login"));
  HttpClient copy(client);
  EXPECT_TRUE(copy.cookieJar().empty());
  EXPECT_EQ(Get(copy, StoreUrl("/echo")), "");
  HttpClient moved(std::move(client));
  EXPECT_EQ(Get(moved, StoreUrl("/echo")), "session=abc;theme=dark");
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_TRUE(client.cookieJar().empty());  // its count included
  HttpClient assigned(Config());
  assigned = std::move(moved);
  EXPECT_EQ(Get(assigned, StoreUrl("/echo")), "session=abc;theme=dark");
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(moved.cookieJar().size(), 0U);
}

constexpr HttpVersionMode kVersionModes[] = {
    HttpVersionMode::Http1_1,
#ifdef AERONET_ENABLE_HTTP2
    HttpVersionMode::Http2,
#endif
};

INSTANTIATE_TEST_SUITE_P(Protocols, HttpClientCookiesTest, ::testing::ValuesIn(kVersionModes),
                         [](const ::testing::TestParamInfo<HttpVersionMode>& info) {
                           return info.param == HttpVersionMode::Http1_1 ? "Http1" : "Http2";
                         });

// HTTP/1.1 only: the cookies spliced into the head along with the "close" connection option, or into the head of a
// chunked request with trailers.
TEST(HttpClientCookiesHttp1Test, CookiesWithConnectionClose) {
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http1_1).withCookies();
  cfg.keepAlive = false;
  HttpClient client(cfg);
  const HttpClientResult login = client.get(StoreUrl("/login"));
  ASSERT_TRUE(login) << ErrcToStr(login.error());
  const HttpClientResult echo = client.get(StoreUrl("/echo-connection"));
  ASSERT_TRUE(echo) << ErrcToStr(echo.error());
  EXPECT_EQ(echo->bodyInMemory(), "session=abc;theme=dark|close");
}

TEST(HttpClientCookiesHttp1Test, CookiesWithTrailers) {
  HttpClientConfig cfg;
  cfg.withHttpVersion(HttpVersionMode::Http1_1).withCookies();
  HttpClient client(cfg);
  ASSERT_TRUE(client.get(StoreUrl("/login")));
  HttpRequest request = client.makeRequest(http::Method::POST, StoreUrl("/echo"), "body");
  request.trailerAddLine("x-checksum", "1");
  const HttpClientResult echo = client.request(request);
  ASSERT_TRUE(echo) << ErrcToStr(echo.error());
  EXPECT_EQ(echo->bodyInMemory(), "session=abc;theme=dark");
}

TEST(HttpClientCookiesConfigTest, PerDomainLimitMustBePositive) {
  HttpClientConfig cfg;
  cfg.withCookies();
  cfg.cookies.maxCookiesPerDomain = 0;
  EXPECT_THROW(static_cast<void>(HttpClient(cfg)), std::invalid_argument);
  cfg.withCookies(0);  // disabled: not checked
  EXPECT_NO_THROW(static_cast<void>(HttpClient(cfg)));
}

TEST(HttpClientCookiesConfigTest, LimitsOfTheJar) {
  HttpClientConfig cfg;
  cfg.withCookies(2);
  cfg.cookies.maxCookiesPerDomain = 2;
  HttpClient client(cfg);
  for (const std::string_view cookie : {"a=1", "b=1", "c=1"}) {
    EXPECT_TRUE(client.cookieJar().setCookie("http://example.com/", cookie));
  }
  EXPECT_EQ(client.cookieJar().size(), 2U);
}

}  // namespace
}  // namespace aeronet
