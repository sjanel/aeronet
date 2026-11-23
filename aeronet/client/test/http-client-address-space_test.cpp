// The address spaces of the requests of web pages (PageRequestContext::addressSpace, Local Network Access): a request
// connects only to the servers of its address space or of a more public one, each redirect only to those of the
// address space of its server or of a more public one, the pooled connections included, and through a forward proxy
// by the host of the URL. On Linux, the connect() override reaches the loopback test server through addresses of the
// other address spaces.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

#ifdef AERONET_LINUX
#define AERONET_WANT_SOCKET_OVERRIDES
#include "aeronet/sys-test-support.hpp"
#endif

#include "aeronet/address-space.hpp"
#include "aeronet/client-connection.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/retry-config.hpp"
#include "aeronet/test_server_fixture.hpp"

namespace aeronet {
namespace {

// The requests received by /count.
std::atomic<int> gCounted{0};

test::TestServer MakeServer() {
  HttpServerConfig cfg;
  cfg.withPort(0).withPollInterval(std::chrono::milliseconds{20});
  test::TestServer server(std::move(cfg));
  auto router = server.router();
  router.setPath(http::Method::GET, "/count", [](const HttpRequestView& req) {
    ++gCounted;
    return req.makeResponse("counted", http::ContentTypeTextPlain);
  });
  router.setPath(http::Method::GET, "/redirect", [](const HttpRequestView& req) {
    HttpResponse resp = req.makeResponse(http::StatusCodeFound);
    resp.location(req.queryParamValue("to").value_or("/"));
    return resp;
  });
  return server;
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
test::TestServer gServer = MakeServer();

// The URL of 'path' on the test server, through 'host'.
std::string Url(std::string_view host, std::string_view path) {
  return "http://" + std::string(host) + ':' + std::to_string(gServer.port()) + std::string(path);
}

// A request of 'url' made for a page of 'addressSpace', with the address space of its final server in
// 'serverAddressSpace'.
HttpClientResult PageRequest(HttpClient& client, std::string_view url, AddressSpace addressSpace,
                             AddressSpace& serverAddressSpace) {
  HttpRequest req = client.makeRequest(http::Method::GET, url);
  return client.requestUpdating(req, {.pAddressSpace = &serverAddressSpace, .addressSpace = addressSpace});
}

HttpClientResult PageRequest(HttpClient& client, std::string_view url, AddressSpace addressSpace) {
  AddressSpace serverAddressSpace = AddressSpace::Loopback;
  return PageRequest(client, url, addressSpace, serverAddressSpace);
}

// The loopback server is reached by the requests that may reach loopback, which tell its address space.
TEST(HttpClientAddressSpaceTest, LoopbackServer) {
  HttpClient client;
  AddressSpace serverAddressSpace = AddressSpace::Public;
  const HttpClientResult result =
      PageRequest(client, Url("127.0.0.1", "/count"), AddressSpace::Loopback, serverAddressSpace);
  ASSERT_TRUE(result) << ErrcToStr(result.error());
  EXPECT_EQ(result->bodyInMemory(), "counted");
  EXPECT_EQ(serverAddressSpace, AddressSpace::Loopback);
}

// The requests of a more public page are refused before any connection, through localhost too.
TEST(HttpClientAddressSpaceTest, LoopbackServerFromMorePublicPages) {
  gCounted = 0;
  HttpClient client;
  for (const AddressSpace addressSpace : {AddressSpace::Local, AddressSpace::Public}) {
    for (const std::string_view host : {"127.0.0.1", "localhost"}) {
      const HttpClientResult result = PageRequest(client, Url(host, "/count"), addressSpace);
      ASSERT_FALSE(result);
      EXPECT_EQ(result.error(), HttpClientErrc::addressBlocked);
    }
  }
  EXPECT_EQ(gCounted, 0);
}

// The other requests reach every server.
TEST(HttpClientAddressSpaceTest, OtherRequestsReachEveryServer) {
  HttpClient client;
  const HttpClientResult result = client.get(Url("127.0.0.1", "/count"));
  ASSERT_TRUE(result) << ErrcToStr(result.error());
  EXPECT_EQ(result->bodyInMemory(), "counted");
}

// A pooled connection to a server less public than allowed is not used, and stays in the pool for the other requests.
TEST(HttpClientAddressSpaceTest, PooledConnectionsAreCheckedToo) {
  gCounted = 0;
  HttpClient client;
  const std::string url = Url("127.0.0.1", "/count");
  ASSERT_TRUE(client.get(url));
  const HttpClientResult blocked = PageRequest(client, url, AddressSpace::Public);
  ASSERT_FALSE(blocked);
  EXPECT_EQ(blocked.error(), HttpClientErrc::addressBlocked);
  EXPECT_TRUE(client.get(url));
  EXPECT_EQ(gCounted, 2);
}

// A refused request is not retried: no address became more public.
TEST(HttpClientAddressSpaceTest, BlockedRequestsAreNotRetried) {
  RetryConfig retry;
  retry.maxAttempts = 3;
  retry.baseDelay = std::chrono::seconds{2};
  retry.maxDelay = std::chrono::seconds{2};
  HttpClientConfig config;
  config.withRetry(retry);
  HttpClient client(config);
  const auto start = std::chrono::steady_clock::now();
  const HttpClientResult result = PageRequest(client, Url("127.0.0.1", "/count"), AddressSpace::Public);
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::addressBlocked);
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{1});
}

// Through a forward proxy (the test server serves the absolute-form requests itself), which resolves the names: the
// host of the URL tells the address space of the server, a name being public.
TEST(HttpClientAddressSpaceTest, ThroughAForwardProxy) {
  gCounted = 0;
  HttpClientConfig config;
  config.withProxy(Url("127.0.0.1", ""));
  HttpClient client(config);
  AddressSpace serverAddressSpace = AddressSpace::Loopback;
  const HttpClientResult named =
      PageRequest(client, "http://example.invalid/count", AddressSpace::Public, serverAddressSpace);
  ASSERT_TRUE(named) << ErrcToStr(named.error());
  EXPECT_EQ(named->bodyInMemory(), "counted");
  EXPECT_EQ(serverAddressSpace, AddressSpace::Public);

  const HttpClientResult local = PageRequest(client, "http://10.0.0.7/count", AddressSpace::Local, serverAddressSpace);
  ASSERT_TRUE(local) << ErrcToStr(local.error());
  EXPECT_EQ(serverAddressSpace, AddressSpace::Local);

  for (const std::string_view url : {
           "http://localhost/count",
           "http://127.0.0.1/count",
           "http://10.0.0.7/count",
           "http://[::1]/count",
           "http://169.254.169.254/count",
           // The other forms of IPv4 addresses that a proxy resolving with getaddrinfo() reads, and the invalid ones.
           "http://127.1/count",
           "http://2130706433/count",
           "http://0x7f.0.0.1/count",
           "http://0177.0.0.1/count",
           "http://169.254.43518/count",
           "http://256.0.0.1/count",
       }) {
    const HttpClientResult blocked = PageRequest(client, url, AddressSpace::Public);
    ASSERT_FALSE(blocked) << url;
    EXPECT_EQ(blocked.error(), HttpClientErrc::addressBlocked) << url;
  }
  EXPECT_EQ(gCounted, 2);

  // A public server does not redirect a request of the application to the host by another form of its address.
  for (const std::string_view to : {"127.1", "0x7f000001", "127.0.0.1."}) {
    const std::string url = "http://example.invalid/redirect?to=" + Url(to, "/count");
    const HttpClientResult blocked = PageRequest(client, url, AddressSpace::Loopback, serverAddressSpace);
    ASSERT_FALSE(blocked) << url;
    EXPECT_EQ(blocked.error(), HttpClientErrc::addressBlocked) << url;
    EXPECT_EQ(serverAddressSpace, AddressSpace::Public);
  }
  EXPECT_EQ(gCounted, 2);
}

#ifdef AERONET_LINUX
// The test server is reached through 203.0.113.7 (public), 0.1.2.3 and 0.1.2.4 (local): none of them is routed, a
// connection to them never leaves the host even without the connect() override.
constexpr std::string_view kPublicHost = "203.0.113.7";
constexpr std::string_view kOtherPublicHost = "203.0.113.8";
constexpr std::string_view kLocalHost = "0.1.2.3";
constexpr std::string_view kOtherLocalHost = "0.1.2.4";

class HttpClientAddressSpaceRedirectedTest : public ::testing::Test {
 protected:
  test::ConnectRedirectionGuard publicHost{kPublicHost};
  test::ConnectRedirectionGuard otherPublicHost{kOtherPublicHost};
  test::ConnectRedirectionGuard localHost{kLocalHost};
  test::ConnectRedirectionGuard otherLocalHost{kOtherLocalHost};
  HttpClient client;
};

TEST_F(HttpClientAddressSpaceRedirectedTest, ServersOfTheAddressSpaceOrOfAMorePublicOne) {
  gCounted = 0;
  struct Case {
    std::string_view host;
    AddressSpace page;
  };
  for (const auto [host, page] : {
           Case{kPublicHost, AddressSpace::Public},
           Case{kPublicHost, AddressSpace::Local},
           Case{kPublicHost, AddressSpace::Loopback},
           Case{kLocalHost, AddressSpace::Local},
           Case{kLocalHost, AddressSpace::Loopback},
       }) {
    AddressSpace serverAddressSpace = AddressSpace::Loopback;
    const HttpClientResult result = PageRequest(client, Url(host, "/count"), page, serverAddressSpace);
    ASSERT_TRUE(result) << host << ": " << ErrcToStr(result.error());
    EXPECT_EQ(serverAddressSpace, host == kPublicHost ? AddressSpace::Public : AddressSpace::Local) << host;
  }
  const HttpClientResult blocked = PageRequest(client, Url(kLocalHost, "/count"), AddressSpace::Public);
  ASSERT_FALSE(blocked);
  EXPECT_EQ(blocked.error(), HttpClientErrc::addressBlocked);
  EXPECT_EQ(gCounted, 5);
}

// A redirect leads only to the servers of the address space of the server that sent it or of a more public one, even
// for a request that may reach every server (a navigation of the application).
TEST_F(HttpClientAddressSpaceRedirectedTest, RedirectsLeadToServersAtLeastAsPublicAsTheirs) {
  gCounted = 0;
  const auto redirect = [](std::string_view from, std::string_view to) {
    return Url(from, "/redirect?to=" + Url(to, "/count"));
  };
  struct Case {
    std::string_view from;
    std::string_view to;
    AddressSpace reached;  // Loopback when blocked
  };
  for (const auto [from, to, reached] : {
           Case{kPublicHost, "127.0.0.1", AddressSpace::Loopback},
           Case{kPublicHost, "localhost", AddressSpace::Loopback},
           Case{kPublicHost, kLocalHost, AddressSpace::Loopback},
           Case{kLocalHost, "127.0.0.1", AddressSpace::Loopback},
           Case{kPublicHost, kOtherPublicHost, AddressSpace::Public},
           Case{kLocalHost, kOtherLocalHost, AddressSpace::Local},
           Case{kLocalHost, kPublicHost, AddressSpace::Public},
           Case{"127.0.0.1", kLocalHost, AddressSpace::Local},
           Case{"127.0.0.1", "localhost", AddressSpace::Loopback},
       }) {
    const bool blocked = reached == AddressSpace::Loopback && from != "127.0.0.1";
    AddressSpace serverAddressSpace = AddressSpace::Loopback;
    const HttpClientResult result = PageRequest(client, redirect(from, to), AddressSpace::Loopback, serverAddressSpace);
    if (blocked) {
      ASSERT_FALSE(result) << from << " -> " << to;
      EXPECT_EQ(result.error(), HttpClientErrc::addressBlocked) << from << " -> " << to;
      // The address space of the server of the redirect.
      EXPECT_EQ(serverAddressSpace, from == kPublicHost ? AddressSpace::Public : AddressSpace::Local);
    } else {
      ASSERT_TRUE(result) << from << " -> " << to << ": " << ErrcToStr(result.error());
      EXPECT_EQ(result->bodyInMemory(), "counted");
      EXPECT_EQ(serverAddressSpace, reached) << from << " -> " << to;
    }
  }
  EXPECT_EQ(gCounted, 5);
}

// With allowLessPublicRedirects, the redirects are only checked against the address space of the request, as by
// browsers: a single sign-on of an intranet through a public identity provider is followed.
TEST_F(HttpClientAddressSpaceRedirectedTest, LessPublicRedirectsWhenAllowed) {
  gCounted = 0;
  const auto request = [this](std::string_view url, AddressSpace addressSpace, bool allowLessPublicRedirects) {
    HttpRequest req = client.makeRequest(http::Method::GET, url);
    return client.requestUpdating(req,
                                  {.addressSpace = addressSpace, .allowLessPublicRedirects = allowLessPublicRedirects});
  };
  const std::string singleSignOn =
      Url(kLocalHost, "/redirect?to=" + Url(kPublicHost, "/redirect?to=" + Url(kOtherLocalHost, "/count")));
  EXPECT_EQ(request(singleSignOn, AddressSpace::Local, false).error(), HttpClientErrc::addressBlocked);
  const HttpClientResult followed = request(singleSignOn, AddressSpace::Local, true);
  ASSERT_TRUE(followed) << ErrcToStr(followed.error());
  EXPECT_EQ(followed->bodyInMemory(), "counted");

  // A navigation of the application, redirected by a public server to the host.
  const std::string toLoopback = Url(kPublicHost, "/redirect?to=" + Url("127.0.0.1", "/count"));
  EXPECT_TRUE(request(toLoopback, AddressSpace::Loopback, true));
  // The address space of the request still applies.
  EXPECT_EQ(request(toLoopback, AddressSpace::Public, true).error(), HttpClientErrc::addressBlocked);
  EXPECT_EQ(gCounted, 2);
}

// The requests that are not made for a page follow every redirect, as before.
TEST_F(HttpClientAddressSpaceRedirectedTest, OtherRequestsFollowEveryRedirect) {
  const HttpClientResult result = client.get(Url(kPublicHost, "/redirect?to=" + Url("127.0.0.1", "/count")));
  ASSERT_TRUE(result) << ErrcToStr(result.error());
  EXPECT_EQ(result->bodyInMemory(), "counted");
}
#endif

}  // namespace
}  // namespace aeronet
