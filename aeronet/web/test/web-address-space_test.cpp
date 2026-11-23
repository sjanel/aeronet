// The address spaces of the requests of the pages (Local Network Access): the URLs given to navigate() within
// WebClientConfig::addressSpace, the requests of the scripts (fetch(), XMLHttpRequest, sendBeacon(), preflights), the
// external scripts and the navigations of a page within its own address space and the more public ones, each redirect
// within the address space of its server and the more public ones. On Linux, the connect() override reaches the
// loopback test server through addresses of the other address spaces; through a forward proxy, the host of the URL
// tells the address space.
#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <string_view>

#ifdef AERONET_LINUX
#define AERONET_WANT_SOCKET_OVERRIDES
#include "aeronet/sys-test-support.hpp"
#endif

#include "aeronet/address-space.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/web-client-config.hpp"
#include "aeronet/web-client.hpp"
#include "aeronet/web-page.hpp"
#include "web-test-util.hpp"

namespace aeronet {
namespace {

// The requests received by /count, and its preflights.
std::atomic<int> gCounted{0};
std::atomic<int> gPreflights{0};

// Makes 'resp' readable by every origin, and consent to the preflights of a PUT with an X-Test header.
void AllowEveryOrigin(HttpResponse& resp) {
  resp.headerAddLine("access-control-allow-origin", "*");
  resp.headerAddLine("access-control-allow-methods", "PUT");
  resp.headerAddLine("access-control-allow-headers", "x-test");
}

test::TestServer MakeServer() {
  test::TestServer server = test::MakePageServer();
  auto router = server.router();
  router.setPath(http::Method::GET | http::Method::PUT | http::Method::POST | http::Method::OPTIONS, "/count",
                 [](const HttpRequestView& req) {
                   if (req.method() == http::Method::OPTIONS) {
                     ++gPreflights;
                   } else {
                     ++gCounted;
                   }
                   HttpResponse resp = req.makeResponse("counted", http::ContentTypeTextPlain);
                   AllowEveryOrigin(resp);
                   return resp;
                 });
  // A redirect to its 'to' query parameter, keeping the method of a PUT (307).
  router.setPath(
      http::Method::GET | http::Method::PUT | http::Method::OPTIONS, "/redirect", [](const HttpRequestView& req) {
        HttpResponse resp = req.makeResponse(http::StatusCodeNoContent);
        if (req.method() != http::Method::OPTIONS) {
          resp.status(req.method() == http::Method::PUT ? http::StatusCodeTemporaryRedirect : http::StatusCodeFound);
          resp.location(req.queryParamValue("to").value_or("/"));
        }
        AllowEveryOrigin(resp);
        return resp;
      });
  return server;
}

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
test::TestServer gServer = MakeServer();

// The URL of 'path' on the test server, through 'host'.
std::string Url(std::string_view host, std::string_view path) {
  return test::ServerUrl(gServer, host) + std::string(path);
}

// The path of a redirect of the test server to 'target'.
std::string RedirectPath(std::string_view target) { return "/redirect?to=" + test::PercentEncode(target); }

// A JavaScript string literal of 'url'.
std::string Quoted(std::string_view url) { return "'" + std::string(url) + "'"; }

// A JavaScript expression of the outcome of fetch() for each of 'urls': the body of its response, or the message of
// its error.
std::string Fetches(std::initializer_list<std::string> urls) {
  std::string code = "Promise.all([";
  for (const std::string& url : urls) {
    code.append("fetch(").append(Quoted(url)).append(").then(r => r.text(), e => e.message),");
  }
  return code + "])";
}

// The configuration of the clients of these tests: a JavaScript call takes much more native stack with the sanitizers
// (see WebNetworkTest).
WebClientConfig Config() {
  WebClientConfig config;
  config.maxScriptStackSize = std::size_t{4} << 20U;
  return config;
}

// The JSON of window.out once the page of the script 'code', served through 'host', and its requests are done.
std::string Out(WebClient& client, std::string_view host, std::string_view code) {
  WebPage page = test::Navigate(client, test::PageUrl(gServer, "<script>" + std::string(code) + "</script>", host),
                                {.until = LoadState::NetworkIdle});
  for (const std::string_view error : page.uncaughtErrors()) {
    ADD_FAILURE() << error;
  }
  return test::Eval(page, "out");
}

// The application reaches the servers of its address space and of the more public ones (WebClientConfig::addressSpace).
TEST(WebAddressSpaceTest, NavigationsOfTheApplication) {
  const std::string url = test::PageUrl(gServer, "<p id=p>");
  WebClient client(Config());
  EXPECT_TRUE(test::Navigate(client, url).document().querySelector("#p"));
  for (const AddressSpace addressSpace : {AddressSpace::Local, AddressSpace::Public}) {
    WebClientConfig config = Config();
    config.addressSpace = addressSpace;
    WebClient restricted(config);
    EXPECT_EQ(restricted.navigate(url).error(), WebErrc::addressBlocked);
    EXPECT_EQ(restricted.navigate(test::PageUrl(gServer, "<p>", "localhost")).error(), WebErrc::addressBlocked);
  }
}

// Through a forward proxy (the test server serves the absolute-form requests itself), which resolves the names, a page
// of a named host is public: it reaches the other names, not the addresses of the host nor localhost.
TEST(WebAddressSpaceTest, ThroughAForwardProxy) {
  gCounted = 0;
  WebClientConfig config = Config();
  config.http.withProxy(test::ServerUrl(gServer));
  WebClient client(config);
  const std::string html = "<script>" +
                           Fetches({
                               "http://other.invalid/count",
                               Url("127.0.0.1", "/count"),
                               Url("localhost", "/count"),
                               "http://10.0.0.7/count",
                           }) +
                           ".then(r => { out = r; })</script>";
  WebPage page = test::Navigate(client, "http://example.invalid/html?src=" + test::PercentEncode(html),
                                {.until = LoadState::NetworkIdle});
  EXPECT_EQ(test::Eval(page, "out"), R"(["counted","Failed to fetch","Failed to fetch","Failed to fetch"])");
  EXPECT_EQ(gCounted, 1);

  config.addressSpace = AddressSpace::Public;
  WebClient restricted(config);
  EXPECT_EQ(restricted.navigate("http://127.0.0.1/html").error(), WebErrc::addressBlocked);
  EXPECT_TRUE(restricted.navigate("http://example.invalid/html"));
}

#ifdef AERONET_LINUX
// The test server is also reached through 203.0.113.7 and 203.0.113.8 (public), 0.1.2.3 and 0.1.2.4 (local): none of
// them is routed, a connection to them never leaves the host even without the connect() override.
constexpr std::string_view kPublicHost = "203.0.113.7";
constexpr std::string_view kOtherPublicHost = "203.0.113.8";
constexpr std::string_view kLocalHost = "0.1.2.3";
constexpr std::string_view kOtherLocalHost = "0.1.2.4";

class WebAddressSpaceRedirectedTest : public ::testing::Test {
 protected:
  test::ConnectRedirectionGuard publicHost{kPublicHost};
  test::ConnectRedirectionGuard otherPublicHost{kOtherPublicHost};
  test::ConnectRedirectionGuard localHost{kLocalHost};
  test::ConnectRedirectionGuard otherLocalHost{kOtherLocalHost};
  WebClient client{Config()};
};

// fetch() reaches the servers of the address space of the page and of the more public ones, the redirects those of the
// address space of their server and of the more public ones.
TEST_F(WebAddressSpaceRedirectedTest, FetchesOfEachAddressSpace) {
  gCounted = 0;
  const std::string toLoopback = RedirectPath(Url("127.0.0.1", "/count"));
  EXPECT_EQ(Out(client, kPublicHost,
                Fetches({Url(kOtherPublicHost, "/count"), Url("127.0.0.1", "/count"), Url("localhost", "/count"),
                         Url(kLocalHost, "/count"), Url(kOtherPublicHost, toLoopback)}) +
                    ".then(r => { out = r; })"),
            R"(["counted","Failed to fetch","Failed to fetch","Failed to fetch","Failed to fetch"])");
  EXPECT_EQ(gCounted, 1);

  EXPECT_EQ(Out(client, kLocalHost,
                Fetches({Url(kOtherLocalHost, "/count"), Url(kPublicHost, "/count"), Url("127.0.0.1", "/count"),
                         Url(kOtherLocalHost, RedirectPath(Url(kLocalHost, "/count"))),
                         Url(kPublicHost, RedirectPath(Url(kLocalHost, "/count")))}) +
                    ".then(r => { out = r; })"),
            R"(["counted","counted","Failed to fetch","counted","Failed to fetch"])");
  EXPECT_EQ(gCounted, 4);

  EXPECT_EQ(Out(client, "127.0.0.1",
                Fetches({Url(kPublicHost, "/count"), Url(kLocalHost, "/count"), Url("localhost", "/count"),
                         Url(kLocalHost, toLoopback)}) +
                    ".then(r => { out = r; })"),
            R"(["counted","counted","counted","Failed to fetch"])");
  EXPECT_EQ(gCounted, 7);
}

// The other requests of a public page to the host: none is sent, not even the preflight.
TEST_F(WebAddressSpaceRedirectedTest, OtherRequestsOfAPublicPage) {
  gCounted = 0;
  gPreflights = 0;
  const std::string loopback = Quoted(Url("127.0.0.1", "/count"));
  EXPECT_EQ(Out(client, kPublicHost,
                "const xhr = new XMLHttpRequest(); xhr.open('GET', " + loopback +
                    ", false); let sync; try { xhr.send(); sync = xhr.status; } catch (e) { sync = e.name; }"
                    "const beacon = navigator.sendBeacon(" +
                    loopback + ", 'b'); fetch(" + loopback +
                    ", { method: 'PUT', headers: { 'X-Test': '1' } }).then(r => r.text(), e => e.message)"
                    ".then(put => { out = [sync, beacon, put]; })"),
            R"(["NetworkError",true,"the CORS preflight of the request to another origin failed"])");
  EXPECT_EQ(gCounted, 0);
  EXPECT_EQ(gPreflights, 0);
}

// A redirect that needs a new CORS preflight stops the chain of the HTTP client, continued after the preflight: within
// the address space of the server of the redirect too, for the preflight and the request, unless
// WebClientConfig::allowLessPublicRedirects.
TEST_F(WebAddressSpaceRedirectedTest, RedirectContinuedAfterAPreflight) {
  const std::string put = "fetch(" + Quoted(Url(kPublicHost, RedirectPath(Url("127.0.0.1", "/count")))) +
                          ", { method: 'PUT', headers: { 'X-Test': '1' } }).then(r => r.text(), e => e.message)"
                          ".then(r => { out = r; })";
  gCounted = 0;
  gPreflights = 0;
  EXPECT_EQ(Out(client, "127.0.0.1", put), R"("the CORS preflight of the request to another origin failed")");
  EXPECT_EQ(gPreflights, 0);
  EXPECT_EQ(gCounted, 0);

  WebClientConfig config = Config();
  config.allowLessPublicRedirects = true;
  WebClient allowing(config);
  EXPECT_EQ(Out(allowing, "127.0.0.1", put), R"("counted")");
  EXPECT_EQ(gPreflights, 1);
  EXPECT_EQ(gCounted, 1);
  // The page still applies.
  EXPECT_EQ(Out(allowing, kLocalHost, put), R"("the CORS preflight of the request to another origin failed")");
  EXPECT_EQ(gPreflights, 1);
  EXPECT_EQ(gCounted, 1);
}

// The external scripts of a page, in its HTML or inserted by its scripts.
TEST_F(WebAddressSpaceRedirectedTest, ScriptsOfAPublicPage) {
  const std::string loopbackScript = Url("127.0.0.1", test::ScriptPath("window.fromLoopback = true"));
  const std::string publicScript = Url(kOtherPublicHost, test::ScriptPath("window.fromPublic = true"));
  WebPage page = test::Navigate(
      client, test::PageUrl(gServer,
                            "<script src='" + loopbackScript + "'></script><script src='" + publicScript +
                                "'></script><script>const s = "
                                "document.createElement('script'); s.src = " +
                                Quoted(loopbackScript) + "; document.head.append(s);</script>",
                            kPublicHost));
  EXPECT_EQ(test::Eval(page, "[window.fromLoopback, window.fromPublic]"), "[null,true]");
}

// A page navigates only to the servers of its address space and of the more public ones, by script or meta refresh,
// and the redirects of a navigation only to those of the address space of their server and of the more public ones.
TEST_F(WebAddressSpaceRedirectedTest, Navigations) {
  const std::string loopbackPage = test::PageUrl(gServer, "<p id=reached>");
  for (const std::string& html : {
           "<script>location.href = " + Quoted(loopbackPage) + "</script>",
           "<meta http-equiv=refresh content='0; url=" + loopbackPage + "'>",
           "<script>location.href = " + Quoted(Url(kOtherPublicHost, RedirectPath(loopbackPage))) + "</script>",
       }) {
    EXPECT_EQ(client.navigate(test::PageUrl(gServer, html, kPublicHost)).error(), WebErrc::addressBlocked) << html;
  }
  // The application reaches every server, but not through the redirect of a public one.
  EXPECT_EQ(client.navigate(Url(kPublicHost, RedirectPath(loopbackPage))).error(), WebErrc::addressBlocked);
  EXPECT_TRUE(
      test::Navigate(client, Url("127.0.0.1", RedirectPath(loopbackPage))).document().querySelector("#reached"));

  // A page of the host navigates to a public page, which keeps the address space of its server.
  const std::string publicPage = test::PageUrl(
      gServer, "<script>" + Fetches({Url("127.0.0.1", "/count")}) + ".then(r => { out = r; })</script>", kPublicHost);
  WebPage page =
      test::Navigate(client, test::PageUrl(gServer, "<script>location.href = " + Quoted(publicPage) + "</script>"),
                     {.until = LoadState::NetworkIdle});
  EXPECT_EQ(page.url(), publicPage);
  EXPECT_EQ(test::Eval(page, "out"), R"(["Failed to fetch"])");
}

// With WebClientConfig::allowLessPublicRedirects, a redirect is only checked against the page, and the URL given to
// navigate() against WebClientConfig::addressSpace, as by browsers: a single sign-on of an intranet through a public
// identity provider is followed.
TEST_F(WebAddressSpaceRedirectedTest, LessPublicRedirectsWhenAllowed) {
  WebClientConfig config = Config();
  config.allowLessPublicRedirects = true;
  WebClient allowing(config);
  const std::string intranetPage = test::PageUrl(gServer, "<p id=reached>", kOtherLocalHost);
  const std::string signOn =
      "<script>location.href = " + Quoted(Url(kPublicHost, RedirectPath(intranetPage))) + "</script>";
  EXPECT_EQ(client.navigate(test::PageUrl(gServer, signOn, kLocalHost)).error(), WebErrc::addressBlocked);
  WebPage page = test::Navigate(allowing, test::PageUrl(gServer, signOn, kLocalHost));
  EXPECT_EQ(page.url(), intranetPage);
  EXPECT_TRUE(page.document().querySelector("#reached"));

  const std::string loopbackPage = test::PageUrl(gServer, "<p id=loopback>");
  EXPECT_TRUE(
      test::Navigate(allowing, Url(kPublicHost, RedirectPath(loopbackPage))).document().querySelector("#loopback"));

  // The page still applies: a public page reaches no local server, even redirected.
  EXPECT_EQ(
      Out(allowing, kPublicHost,
          Fetches({Url(kOtherPublicHost, RedirectPath(Url("127.0.0.1", "/count")))}) + ".then(r => { out = r; })"),
      R"(["Failed to fetch"])");
}
#endif

}  // namespace
}  // namespace aeronet
