#include <gtest/gtest.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

namespace {
// Use a short poll interval so the server's periodic maintenance (which enforces
// header read timeouts) runs promptly even when the test runner is under heavy load.
// This avoids flakiness when the whole test suite is executed in parallel.
test::TestServer ts(HttpServerConfig{}, RouterConfig{}, std::chrono::milliseconds{5});
auto port = ts.port();

struct HeaderReadTimeoutScope {
  explicit HeaderReadTimeoutScope(std::chrono::milliseconds timeout) {
    ts.postConfigUpdate([timeout](HttpServerConfig& cfg) { cfg.withHeaderReadTimeout(timeout); });
  }

  HeaderReadTimeoutScope(const HeaderReadTimeoutScope&) = delete;
  HeaderReadTimeoutScope& operator=(const HeaderReadTimeoutScope&) = delete;
  HeaderReadTimeoutScope(HeaderReadTimeoutScope&&) = delete;
  HeaderReadTimeoutScope& operator=(HeaderReadTimeoutScope&&) = delete;

  ~HeaderReadTimeoutScope() {
    ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withHeaderReadTimeout(std::chrono::milliseconds{0}); });
  }
};

struct MaxPerEventReadBytesScope {
  explicit MaxPerEventReadBytesScope(std::size_t limitBytes) : _previous(ts.server.config().maxPerEventReadBytes) {
    ts.postConfigUpdate([limitBytes](HttpServerConfig& cfg) { cfg.withMaxPerEventReadBytes(limitBytes); });
  }
  MaxPerEventReadBytesScope(const MaxPerEventReadBytesScope&) = delete;
  MaxPerEventReadBytesScope& operator=(const MaxPerEventReadBytesScope&) = delete;
  MaxPerEventReadBytesScope(MaxPerEventReadBytesScope&&) = delete;
  MaxPerEventReadBytesScope& operator=(MaxPerEventReadBytesScope&&) = delete;

  ~MaxPerEventReadBytesScope() {
    ts.postConfigUpdate([prev = _previous](HttpServerConfig& cfg) { cfg.withMaxPerEventReadBytes(prev); });
  }

 private:
  std::size_t _previous;
};

struct TcpNoDelayScope {
  explicit TcpNoDelayScope(bool enabled) : _previous(ts.server.config().tcpNoDelay) {
    ts.postConfigUpdate([enabled](HttpServerConfig& cfg) { cfg.withTcpNoDelay(enabled); });
  }
  TcpNoDelayScope(const TcpNoDelayScope&) = delete;
  TcpNoDelayScope& operator=(const TcpNoDelayScope&) = delete;
  TcpNoDelayScope(TcpNoDelayScope&&) = delete;
  TcpNoDelayScope& operator=(TcpNoDelayScope&&) = delete;

  ~TcpNoDelayScope() {
    ts.postConfigUpdate([prev = _previous](HttpServerConfig& cfg) { cfg.withTcpNoDelay(prev); });
  }

 private:
  bool _previous;
};

std::string httpGet(uint16_t port, std::string_view target) {
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = target;
  opt.connection = "close";
  opt.headers.emplace_back("X-Test", "abc123");
  auto resp = test::request(port, opt);
  return resp.value_or("");
}

}  // namespace

TEST(HttpHeadersCustom, ForwardsSingleAndMultipleCustomHeaders) {
  ts.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.status(201).reason("Created");
    resp.header("X-One", "1");
    resp.header("X-Two", "two");
    resp.body("B");
    return resp;
  });
  test::ClientConnection cc(ts.port());
  int fd = cc.fd();
  std::string req = "GET /h HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("201 Created"));
  ASSERT_TRUE(resp.contains("X-One: 1"));
  ASSERT_TRUE(resp.contains("X-Two: two"));
  ASSERT_TRUE(resp.contains("Content-Length: 1"));  // auto generated
  ASSERT_TRUE(resp.contains("Connection: close"));  // auto generated (keep-alive or close)
}

TEST(HttpHeadersCustom, LocationHeaderAllowed) {
  ts.router().setDefault([](const HttpRequest&) {
    HttpResponse resp(302, "Found");
    resp.location("/new").body("");
    return resp;
  });
  test::ClientConnection cc(ts.port());
  int fd = cc.fd();
  std::string req = "GET /h HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("302 Found"));
  ASSERT_TRUE(resp.contains("Location: /new"));
}

TEST(HttpHeadersCustom, CaseInsensitiveReplacementPreservesFirstCasing) {
  // Verify that calling customHeader with different casing replaces existing value without duplicating the line and
  // preserves the original header name casing from the first insertion.
  ts.router().setDefault([](const HttpRequest&) {
    HttpResponse resp;
    resp.header("x-cAsE", "one");
    resp.header("X-Case", "two");    // should replace value only
    resp.header("X-CASE", "three");  // replace again
    resp.body("b");
    return resp;
  });
  test::ClientConnection cc(ts.port());
  int fd = cc.fd();
  std::string req = "GET /h HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string responseText = test::recvUntilClosed(fd);
  // Expect only one occurrence with original first casing and final value 'three'.
  ASSERT_TRUE(responseText.contains("x-cAsE: three")) << responseText;
  EXPECT_FALSE(responseText.contains("X-Case:")) << responseText;
  EXPECT_FALSE(responseText.contains("X-CASE: three")) << responseText;
}

TEST(HttpServerConfigLimits, MaxPerEventReadBytesAppliesAtRuntime) {
  const std::size_t cap = ts.server.config().initialReadChunkBytes * 2;
  MaxPerEventReadBytesScope scope(cap);

  const std::size_t payloadSize = cap * 3;
  std::string payload(payloadSize, 'x');
  ts.router().setDefault([payloadSize](const HttpRequest& req) {
    HttpResponse resp;
    if (req.body().size() != payloadSize) {
      resp.status(http::StatusCodeBadRequest).body("payload mismatch");
    } else {
      resp.body("payload ok");
    }
    return resp;
  });

  test::ClientConnection cc(ts.port());
  int fd = cc.fd();
  std::string header = "POST /fairness HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(payloadSize) +
                       "\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, header);
  const auto chunkDelay = ts.server.config().pollInterval + 10ms;
  for (std::size_t sent = 0; sent < payload.size();) {
    std::this_thread::sleep_for(chunkDelay);
    const std::size_t chunkSize = std::min(cap, payload.size() - sent);
    test::sendAll(fd, std::string_view(payload.data() + sent, chunkSize));
    sent += chunkSize;
  }

  std::string resp = test::recvUntilClosed(fd);
  ASSERT_FALSE(resp.empty()) << "expected a response";
  ASSERT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("payload ok")) << resp;
}

TEST(HttpServerConfig, TcpNoDelayEnablesSimpleGet) {
  TcpNoDelayScope scope(true);
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK).body("tcp ok"); });
  std::string resp = httpGet(ts.port(), "/tcp");
  ASSERT_FALSE(resp.empty());
  ASSERT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("tcp ok")) << resp;
}

TEST(HttpHeaderTimeout, Emits408WhenHeadersCompletedAfterDeadline) {
  static constexpr std::chrono::milliseconds readTimeout = std::chrono::milliseconds{50};
  HeaderReadTimeoutScope headerTimeout(readTimeout);

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, "OK").body("hi"); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  test::ClientConnection cnx(ts.port());
  int fd = cnx.fd();
  ASSERT_GE(fd, 0) << "connect failed";
  // Send an incomplete request line and let it stall past the timeout.
  test::sendAll(fd, "GET /", std::chrono::milliseconds{100});
  std::this_thread::sleep_for(readTimeout + std::chrono::milliseconds{10});
  // Try to finish the request; the server should already consider it timed out and reply with 408.
  static constexpr std::string_view rest = " HTTP/1.1\r\nHost: x\r\n\r\n";
  (void)::send(fd, rest.data(), rest.size(), MSG_NOSIGNAL);

  std::string resp = test::recvWithTimeout(fd, std::chrono::milliseconds{300});
  ASSERT_FALSE(resp.empty());
  EXPECT_TRUE(resp.contains("HTTP/1.1 408")) << resp;
  EXPECT_TRUE(resp.contains("Connection: close")) << resp;
}

TEST(HttpHeaderTimeout, Emits408WhenHeadersNeverComplete) {
  static constexpr std::chrono::milliseconds readTimeout = std::chrono::milliseconds{50};
  HeaderReadTimeoutScope headerTimeout(readTimeout);

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, "OK").body("hi"); });
  test::ClientConnection cnx(ts.port());
  int fd = cnx.fd();
  ASSERT_GE(fd, 0) << "connect failed";

  test::sendAll(fd, "POST ", std::chrono::milliseconds{100});
  std::this_thread::sleep_for(readTimeout + std::chrono::milliseconds{20});

  std::string resp = test::recvWithTimeout(fd, std::chrono::milliseconds{300});
  ASSERT_FALSE(resp.empty());
  EXPECT_TRUE(resp.contains("HTTP/1.1 408")) << resp;
  EXPECT_TRUE(resp.contains("Connection: close")) << resp;
}

TEST(HttpBasic, SimpleGet) {
  ts.router().setDefault([](const HttpRequest& req) {
    HttpResponse resp;
    auto testHeaderIt = req.headers().find("X-Test");
    std::string body("You requested: ");
    body += req.path();
    if (testHeaderIt != req.headers().end() && !testHeaderIt->second.empty()) {
      body += ", X-Test=";
      body.append(testHeaderIt->second);
    }
    resp.body(std::move(body));
    return resp;
  });
  std::string resp = httpGet(ts.port(), "/abc");
  ASSERT_FALSE(resp.empty());
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("You requested: /abc"));
  ASSERT_TRUE(resp.contains("X-Test=abc123"));
}

TEST(HttpKeepAlive, MultipleSequentialRequests) {
  ts.router().setDefault([](const HttpRequest& req) {
    HttpResponse resp;
    resp.body(std::string("ECHO") + std::string(req.path()));
    return resp;
  });

  test::ClientConnection cnx(port);
  int fd = cnx.fd();

  std::string req1 = "GET /one HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n";
  test::sendAll(fd, req1);
  std::string resp1 = test::recvWithTimeout(fd);
  EXPECT_TRUE(resp1.contains("ECHO/one"));
  EXPECT_FALSE(resp1.contains("Connection: close"));

  std::string req2 = "GET /two HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";  // implicit keep-alive
  test::sendAll(fd, req2);
  std::string resp2 = test::recvWithTimeout(fd);
  EXPECT_TRUE(resp2.contains("ECHO/two"));
  EXPECT_FALSE(resp2.contains("Connection: close"));
}

namespace {
std::string rawGet(uint16_t port) {
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/";
  opt.connection = "close";
  auto resp = test::request(port, opt);
  return resp.value_or("");
}

std::string headerValue(std::string_view resp, std::string_view name) {
  std::string needle = std::string(name) + ": ";
  std::size_t pos = resp.find(needle);
  if (pos == std::string_view::npos) {
    return {};
  }
  std::size_t end = resp.find(http::CRLF, pos);
  if (end == std::string_view::npos) {
    return {};
  }
  return std::string(resp.substr(pos + needle.size(), end - (pos + needle.size())));
}

}  // namespace

TEST(HttpDate, PresentAndFormat) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });
  auto resp = rawGet(port);
  ASSERT_FALSE(resp.empty());
  auto date = headerValue(resp, "Date");
  ASSERT_EQ(29U, date.size()) << date;
  std::regex re("[A-Z][a-z]{2}, [0-9]{2} [A-Z][a-z]{2} [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT");
  ASSERT_TRUE(std::regex_match(date, re)) << date;
}

TEST(HttpDate, StableWithinSameSecond) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

  // To avoid flakiness near a second rollover on slower / contended CI hosts:
  // Probe until the current second is "stable" for at least ~20ms before sampling sequence.
  // (Single RFC7231 date string has fixed length; we extract the HH:MM:SS portion.)
  auto extractHMS = [](std::string_view dateHeader) -> std::string {
    if (dateHeader.size() < 29) {
      return {};
    }
    return std::string(
        dateHeader.substr(17, 8));  // positions of HH:MM:SS in RFC7231 format: "Wdy, DD Mon YYYY HH:MM:SS GMT"
  };

  std::string anchorDate;
  std::string anchorHMS;
  for (int i = 0; i < 50; ++i) {  // up to ~500ms budget
    anchorDate = headerValue(rawGet(port), "Date");
    anchorHMS = extractHMS(anchorDate);
    if (!anchorHMS.empty()) {
      // Sleep a short time and confirm we are still in same second; if not, loop and pick new anchor.
      std::this_thread::sleep_for(20ms);
      auto confirm = headerValue(rawGet(port), "Date");
      if (extractHMS(confirm) == anchorHMS) {
        anchorDate = confirm;  // use the confirmed value
        break;
      }
    }
  }
  ASSERT_FALSE(anchorDate.empty());

  // Take two additional samples and ensure at least two out of the three share the same second.
  // (If we landed exactly on a boundary the anchor may differ, but then the other two should match.)
  auto s2 = headerValue(rawGet(port), "Date");
  auto s3 = headerValue(rawGet(port), "Date");
  std::string h1 = extractHMS(anchorDate);
  std::string h2 = extractHMS(s2);
  std::string h3 = extractHMS(s3);

  int pairs = 0;
  pairs += (h1 == h2) ? 1 : 0;
  pairs += (h1 == h3) ? 1 : 0;
  pairs += (h2 == h3) ? 1 : 0;
  // IMPORTANT: Stop server before the assertion so a failure does not leave the thread running.
  // (ASSERT_* aborts the test function; previously this caused a 300s timeout in CI because the
  // predicate-controlled loop never observed stop=true.)
  ASSERT_GE(pairs, 1) << "Too much drift across second boundaries: '" << anchorDate << "' '" << s2 << "' '" << s3
                      << "'";
}

TEST(HttpDate, ChangesAcrossSecondBoundary) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

  auto first = rawGet(port);
  auto d1 = headerValue(first, "Date");
  ASSERT_EQ(29U, d1.size());
  // spin until date changes (max ~1500ms)
  std::string d2;
  for (int i = 0; i < 150; ++i) {
    std::this_thread::sleep_for(10ms);
    d2 = headerValue(rawGet(port), "Date");
    if (d2 != d1 && !d2.empty()) {
      break;
    }
  }
  ASSERT_NE(d1, d2) << "Date header did not change across boundary after waiting";
}

struct ErrorCase {
  const char* name;
  const char* request;
  const char* expectedStatus;  // substring (e.g. "400", "505")
};

class HttpErrorParamTest : public ::testing::TestWithParam<ErrorCase> {};

TEST_P(HttpErrorParamTest, EmitsExpectedStatus) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });
  const auto& param = GetParam();
  std::string resp = test::sendAndCollect(port, param.request);
  ASSERT_TRUE(resp.contains(param.expectedStatus)) << "Case=" << param.name << "\nResp=" << resp;
}

INSTANTIATE_TEST_SUITE_P(
    HttpErrors, HttpErrorParamTest,
    ::testing::Values(ErrorCase{"MalformedRequestLine", "GETONLYNOPATH\r\n\r\n", "400"},
                      ErrorCase{"VersionNotSupported", "GET /test HTTP/2.0\r\nHost: x\r\n\r\n", "505"},
                      ErrorCase{"UnsupportedTransferEncoding",
                                "POST /u HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\nConnection: close\r\n\r\n",
                                "501"},
                      ErrorCase{"ContentLengthTransferEncodingConflict",
                                "POST /c HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: "
                                "chunked\r\nConnection: close\r\n\r\nhello",
                                "400"}));

TEST(HttpKeepAlive10, DefaultCloseWithoutHeader) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse().body("ok"); });
  // HTTP/1.0 without Connection: keep-alive should close
  test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string_view req = "GET /h HTTP/1.0\r\nHost: x\r\n\r\n";
  test::sendAll(fd, req);

  std::string resp = test::recvUntilClosed(fd);

  ASSERT_FALSE(resp.contains("Connection:"));
  // Second request should not yield another response (connection closed). We attempt to read after sending.
  std::string_view req2 = "GET /h2 HTTP/1.0\r\nHost: x\r\n\r\n";
  test::sendAll(fd, req2);
  // Expect no data (connection should be closed) -- use test helper which waits briefly
  auto n2 = test::recvWithTimeout(fd);
  EXPECT_TRUE(n2.empty());
}

TEST(HttpKeepAlive10, OptInWithHeader) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse().body("ok"); });
  test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string_view req = "GET /h HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  test::sendAll(fd, req);
  std::string first = test::recvWithTimeout(fd);
  ASSERT_TRUE(first.contains("Connection: keep-alive"));
  std::string_view req2 = "GET /h2 HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  test::sendAll(fd, req2);
  std::string second = test::recvWithTimeout(fd);
  ASSERT_TRUE(second.contains("Connection: keep-alive"));
}

namespace {
std::string sendRaw(uint16_t port, std::string_view raw) {
  test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  test::sendAll(fd, raw);
  std::string resp = test::recvWithTimeout(fd, 300ms);
  // server may close depending on error severity
  return resp;
}
}  // anonymous namespace

TEST(HttpMalformed, MissingSpacesInRequestLine) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });
  std::string resp = sendRaw(port, "GET/abcHTTP/1.1\r\nHost: x\r\n\r\n");
  ASSERT_TRUE(resp.contains("400")) << resp;
}

TEST(HttpMalformed, OversizedHeaders) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxHeaderBytes(128); });
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

  std::string big(200, 'A');
  std::string raw = "GET / HTTP/1.1\r\nHost: x\r\nX-Big: " + big + "\r\n\r\n";
  std::string resp = sendRaw(port, raw);
  ASSERT_TRUE(resp.contains("431")) << resp;
}

TEST(HttpMalformed, BadChunkExtensionHex) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

  // Transfer-Encoding with invalid hex char 'Z'
  std::string raw = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\n";  // incomplete + invalid
  std::string resp = sendRaw(port, raw);
  // Expect no 200 OK; either empty (waiting for more) or eventually 413/400 once completed; we at least assert not 200
  ASSERT_FALSE(resp.contains("200 OK"));
}

TEST(HttpMethodParsing, AcceptsCaseInsensitiveMethodTokens) {
  // Ensure the server accepts method tokens in mixed case (robustness per RFC 9110 §2.5).
  ts.router().setDefault([](const HttpRequest& req) {
    HttpResponse resp;
    // Echo the canonical method name (parser maps mixed-case to enum).
    resp.body(std::string("method=") + std::string(http::MethodToStr(req.method())));
    return resp;
  });

  // Representative variants for common methods.
  std::vector<std::pair<std::string, std::string>> cases = {
      {"GET /ci HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", "GET"},
      {"get /ci HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", "GET"},
      {"GeT /ci HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", "GET"},
      {"POST /ci HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", "POST"},
      {"pOsT /ci HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", "POST"},
  };

  for (const auto& pair : cases) {
    std::string resp = sendRaw(port, pair.first);
    // Response should be 200 and include the method echoed in the body.
    EXPECT_TRUE(resp.contains("HTTP/1.1 200")) << "Resp=" << resp;
    EXPECT_TRUE(resp.contains(std::string("method=") + pair.second)) << "Resp=" << resp;
  }
}

TEST(HttpServerCopy, CopyConstruct) {
  // Copy-constructing from a stopped server should duplicate configuration/router but not runtime state.
  HttpServerConfig cfg;
  cfg.withReusePort();

  Router router;
  router.setDefault([](const HttpRequest& req) {
    HttpResponse resp;
    resp.body(std::string("ORIG:") + std::string(req.path()));
    return resp;
  });

  HttpServer origin(cfg, std::move(router));

  auto origPort = origin.port();

  // Copy construct while stopped is fine.
  HttpServer copy(origin);

  origin.stop();  // ensure we stop listener on the original server to avoid queries reaching this server

  copy.start();

  // Start the copy in background and exercise the handler on the original port.
  std::string resp = test::simpleGet(origPort, "/copy");
  ASSERT_TRUE(resp.contains("ORIG:/copy"));

  EXPECT_THROW(HttpServer{copy}, std::logic_error) << "Copy-constructing from a running server should throw";
}
