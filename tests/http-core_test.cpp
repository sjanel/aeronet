#include <gtest/gtest.h>

#ifdef AERONET_POSIX
#include <sys/socket.h>
#elifdef AERONET_WINDOWS
#include <winsock2.h>
#endif

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <ranges>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "aeronet/close-native-handle.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/features.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/rate-limit-middleware.hpp"
#include "aeronet/rate-limit.hpp"
#include "aeronet/request-metrics.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/static-file-handler.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/unix-dogstatsd-sink.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/zerocopy-mode.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/compression-test-helpers.hpp"
#endif

using namespace std::chrono_literals;

namespace aeronet {

namespace {
test::TestServer ts;
const auto port = ts.port();

using HeaderReadTimeoutScope = test::ScopedConfigUpdate<std::chrono::milliseconds>;
auto makeHeaderReadTimeoutScope(std::chrono::milliseconds timeout) {
  return HeaderReadTimeoutScope(
      ts, [](const HttpServerConfig& config) { return config.headerReadTimeout; },
      [](HttpServerConfig& config, std::chrono::milliseconds timeout) { config.withHeaderReadTimeout(timeout); },
      timeout);
}

using MaxPerEventReadBytesScope = test::ScopedConfigUpdate<std::size_t>;
auto makeMaxPerEventReadBytesScope(std::size_t limitBytes) {
  return MaxPerEventReadBytesScope(
      ts, [](const HttpServerConfig& config) { return config.maxPerEventReadBytes; },
      [](HttpServerConfig& config, std::size_t limitBytes) { config.withMaxPerEventReadBytes(limitBytes); },
      limitBytes);
}

using TcpNoDelayScope = test::ScopedConfigUpdate<TcpNoDelayMode>;
auto makeTcpNoDelayScope(TcpNoDelayMode mode) {
  return TcpNoDelayScope(
      ts, [](const HttpServerConfig& config) { return config.tcpNoDelay; },
      [](HttpServerConfig& config, TcpNoDelayMode mode) { config.withTcpNoDelayMode(mode); }, mode);
}

std::string httpGet(std::string_view target) {
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
  ts.router().setDefault([](const HttpRequestView&) {
    HttpResponse resp;
    resp.status(201).reason("Created");
    resp.header("X-One", "1");
    resp.header("X-Two", "two");
    resp.body("B");
    return resp;
  });
  test::ClientConnection cc(ts.port());
  NativeHandle fd = cc.fd();
  std::string req = "GET /h HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 201 Created"));
  ASSERT_TRUE(resp.contains("X-One: 1"));
  ASSERT_TRUE(resp.contains("X-Two: two"));
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentLength, "1")));   // auto generated
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::Connection, "close")));  // auto generated (keep-alive or close)
}

TEST(HttpHeadersCustom, LocationHeaderAllowed) {
  ts.router().setDefault([](const HttpRequestView&) {
    HttpResponse resp(302);
    resp.reason("Found");
    resp.location("/new").body("");
    return resp;
  });
  test::ClientConnection cc(ts.port());
  NativeHandle fd = cc.fd();
  std::string req = "GET /h HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 302 Found"));
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::Location, "/new")));
}

TEST(HttpHeadersCustom, CaseInsensitiveReplacementPreservesFirstCasing) {
  // Verify that calling customHeader with different casing replaces existing value without duplicating the line and
  // preserves the original header name casing from the first insertion.
  ts.router().setDefault([](const HttpRequestView&) {
    HttpResponse resp;
    resp.header("x-cAsE", "one");
    resp.header("X-Case", "two");    // should replace value only
    resp.header("X-CASE", "three");  // replace again
    resp.body("b");
    return resp;
  });
  test::ClientConnection cc(ts.port());
  NativeHandle fd = cc.fd();
  std::string req = "GET /h HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string responseText = test::recvUntilClosed(fd);
  // Expect only one occurrence with original first casing and final value 'three'.
  ASSERT_TRUE(responseText.contains("x-cAsE: three")) << responseText;
  EXPECT_FALSE(responseText.contains("X-Case:")) << responseText;
  EXPECT_FALSE(responseText.contains("X-CASE: three")) << responseText;
}

TEST(HttpServerConfigLimits, MaxPerEventReadBytesAppliesAtRuntime) {
  const std::size_t cap = 2UL * ts.server.config().minReadChunkBytes;
  auto scope = makeMaxPerEventReadBytesScope(cap);

  const std::size_t payloadSize = cap * 3;
  std::string payload(payloadSize, 'x');
  ts.router().setDefault([payloadSize](const HttpRequestView& req) {
    HttpResponse resp;
    if (req.body().size() != payloadSize) {
      resp.status(http::StatusCodeBadRequest).body("payload mismatch");
    } else {
      resp.body("payload ok");
    }
    return resp;
  });

  test::ClientConnection cc(ts.port());
  NativeHandle fd = cc.fd();
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
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("payload ok")) << resp;
}

TEST(HttpServerConfigLimits, DeferredReadDrainsBufferAfterFairnessCap) {
  // Send header + body in a single write so the TCP stack coalesces everything
  // into one kernel buffer. With edge-triggered polling the fd won't fire again
  // after the fairness cap breaks the read loop, so the deferred-read path must kick in.
  const std::size_t cap = ts.server.config().minReadChunkBytes;
  auto scope = makeMaxPerEventReadBytesScope(cap);

  const std::size_t payloadSize = cap * 4;
  std::string payload(payloadSize, 'y');
  ts.router().setDefault([payloadSize](const HttpRequestView& req) {
    HttpResponse resp;
    if (req.body().size() != payloadSize) {
      resp.status(http::StatusCodeBadRequest).body("size mismatch");
    } else {
      resp.body("deferred ok");
    }
    return resp;
  });

  std::string raw = "POST /deferred HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(payloadSize) +
                    "\r\nConnection: close\r\n\r\n" + payload;
  std::string resp = test::sendAndCollect(port, raw);
  ASSERT_FALSE(resp.empty()) << "expected a response";
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("deferred ok")) << resp;
}

TEST(HttpServerConfigLimits, DeferredReadHandlesDisconnectedFd) {
  // Verify that a fd closed between being deferred and being re-read
  // is silently skipped (the IsValid check in the deferred loop).
  const std::size_t cap = ts.server.config().minReadChunkBytes;
  auto scope = makeMaxPerEventReadBytesScope(cap);

  const std::size_t payloadSize = cap * 4;
  std::string payload(payloadSize, 'z');
  ts.router().setDefault([payloadSize](const HttpRequestView& req) {
    HttpResponse resp;
    resp.body(req.body().size() == payloadSize ? "ok" : "bad");
    return resp;
  });

  // First request: succeed normally to verify the cap path works.
  std::string raw = "POST /deferred2 HTTP/1.1\r\nHost: x\r\nContent-Length: " + std::to_string(payloadSize) +
                    "\r\nConnection: close\r\n\r\n" + payload;
  std::string resp = test::sendAndCollect(port, raw);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("ok")) << resp;
}

TEST(HttpServerConfig, TcpNoDelayEnablesSimpleGet) {
  auto scope = makeTcpNoDelayScope(TcpNoDelayMode::Enabled);
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("tcp ok"); });
  std::string resp = httpGet("/tcp");
  ASSERT_FALSE(resp.empty());
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("tcp ok")) << resp;
}

TEST(HttpHeaderTimeout, Emits408WhenHeadersCompletedAfterDeadline) {
  static constexpr std::chrono::milliseconds readTimeout = std::chrono::milliseconds{50};
  auto headerTimeout = makeHeaderReadTimeoutScope(readTimeout);

  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("hi"); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  test::ClientConnection cnx(ts.port());
  NativeHandle fd = cnx.fd();
  ASSERT_GE(fd, 0) << "connect failed";
  // Send an incomplete request line and let it stall past the timeout.
  test::sendAll(fd, "GET /", std::chrono::milliseconds{100});
  std::this_thread::sleep_for(readTimeout + std::chrono::milliseconds{20});

  // Read the 408 BEFORE sending more data — on Windows, sending to a peer that has
  // closed can trigger RST processing which discards pending receive-buffer data.
  std::string resp = test::recvWithTimeout(fd, std::chrono::milliseconds{500});

  // Try to finish the request; the server should already consider it timed out.
  static constexpr std::string_view rest = " HTTP/1.1\r\nHost: x\r\n\r\n";
  SafeSend(fd, rest.data(), rest.size());

  ASSERT_FALSE(resp.empty());
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 408")) << resp;
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine(http::Connection, "close"))) << resp;
}

TEST(HttpHeaderTimeout, Emits408WhenHeadersNeverComplete) {
  static constexpr std::chrono::milliseconds readTimeout = std::chrono::milliseconds{50};
  auto headerTimeout = makeHeaderReadTimeoutScope(readTimeout);

  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("hi"); });
  test::ClientConnection cnx(ts.port());
  NativeHandle fd = cnx.fd();
  ASSERT_GE(fd, 0) << "connect failed";

  test::sendAll(fd, "POST ", std::chrono::milliseconds{100});
  std::this_thread::sleep_for(readTimeout + std::chrono::milliseconds{20});

  std::string resp = test::recvWithTimeout(fd, std::chrono::milliseconds{300});
  ASSERT_FALSE(resp.empty());
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 408")) << resp;
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine(http::Connection, "close"))) << resp;
}

TEST(HttpBasic, SimpleGet) {
  ts.router().setDefault([](const HttpRequestView& req) {
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
  std::string resp = httpGet("/abc");
  ASSERT_FALSE(resp.empty());
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("You requested: /abc"));
  ASSERT_TRUE(resp.contains("X-Test=abc123"));
}

TEST(HttpKeepAlive, MultipleSequentialRequests) {
  ts.router().setDefault([](const HttpRequestView& req) {
    HttpResponse resp;
    resp.body(std::string("ECHO") + std::string(req.path()));
    return resp;
  });

  test::ClientConnection cnx(port);
  NativeHandle fd = cnx.fd();

  std::string req1 = "GET /one HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n";
  test::sendAll(fd, req1);
  std::string resp1 = test::recvWithTimeout(fd);
  EXPECT_TRUE(resp1.contains("ECHO/one"));
  EXPECT_FALSE(resp1.contains(MakeHttp1HeaderLine(http::Connection, "close")));

  std::string req2 = "GET /two HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";  // implicit keep-alive
  test::sendAll(fd, req2);
  std::string resp2 = test::recvWithTimeout(fd);
  EXPECT_TRUE(resp2.contains("ECHO/two"));
  EXPECT_FALSE(resp2.contains(MakeHttp1HeaderLine(http::Connection, "close")));
}

TEST(HttpKeepAlive, EmptyBodyResponseCarriesContentLengthZeroAndReusesConnection) {
  // An empty-body response must declare Content-Length: 0 so a keep-alive client can frame the (zero-length)
  // body immediately and reuse the connection, instead of waiting for the idle timeout / connection close.
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK); });  // empty body

  test::ClientConnection cnx(port);
  NativeHandle fd = cnx.fd();

  std::string req1 = "GET /a HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n";
  test::sendAll(fd, req1);
  std::string resp1 = test::recvWithTimeout(fd);
  EXPECT_TRUE(resp1.starts_with("HTTP/1.1 200")) << resp1;
  EXPECT_TRUE(resp1.contains(MakeHttp1HeaderLine(http::ContentLength, "0"))) << resp1;
  EXPECT_FALSE(resp1.contains(MakeHttp1HeaderLine(http::Connection, "close"))) << resp1;
  EXPECT_TRUE(resp1.ends_with(http::DoubleCRLF)) << resp1;

  // The connection is immediately reusable: a second request on the same fd is served.
  std::string req2 = "GET /b HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";  // implicit keep-alive
  test::sendAll(fd, req2);
  std::string resp2 = test::recvWithTimeout(fd);
  EXPECT_TRUE(resp2.starts_with("HTTP/1.1 200")) << resp2;
  EXPECT_TRUE(resp2.contains(MakeHttp1HeaderLine(http::ContentLength, "0"))) << resp2;
}

namespace {
std::string rawGet() {
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
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK); });
  auto resp = rawGet();
  ASSERT_FALSE(resp.empty());
  auto date = headerValue(resp, http::Date);
  ASSERT_EQ(29U, date.size()) << date;
  std::regex re("[A-Z][a-z]{2}, [0-9]{2} [A-Z][a-z]{2} [0-9]{4} [0-9]{2}:[0-9]{2}:[0-9]{2} GMT");
  ASSERT_TRUE(std::regex_match(date, re)) << date;
}

TEST(HttpDate, StableWithinSameSecond) {
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK); });

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
    anchorDate = headerValue(rawGet(), http::Date);
    anchorHMS = extractHMS(anchorDate);
    if (!anchorHMS.empty()) {
      // Sleep a short time and confirm we are still in same second; if not, loop and pick new anchor.
      std::this_thread::sleep_for(20ms);
      auto confirm = headerValue(rawGet(), http::Date);
      if (extractHMS(confirm) == anchorHMS) {
        anchorDate = confirm;  // use the confirmed value
        break;
      }
    }
  }
  ASSERT_FALSE(anchorDate.empty());

  // Take two additional samples and ensure at least two out of the three share the same second.
  // (If we landed exactly on a boundary the anchor may differ, but then the other two should match.)
  auto s2 = headerValue(rawGet(), http::Date);
  auto s3 = headerValue(rawGet(), http::Date);
  std::string h1 = extractHMS(anchorDate);
  std::string h2 = extractHMS(s2);
  std::string h3 = extractHMS(s3);

  int pairs = 0;
  pairs += (h1 == h2) ? 1 : 0;
  pairs += (h1 == h3) ? 1 : 0;
  pairs += (h2 == h3) ? 1 : 0;

  ASSERT_GE(pairs, 1) << "Too much drift across second boundaries: '" << anchorDate << "' '" << s2 << "' '" << s3
                      << "'";
}

TEST(HttpDate, ChangesAcrossSecondBoundary) {
  ts.router().setDefault([](const HttpRequestView& req) { return req.makeResponse(http::StatusCodeOK); });

  auto first = rawGet();
  auto d1 = headerValue(first, http::Date);
  ASSERT_EQ(29U, d1.size());
  // spin until date changes (max ~1500ms)
  std::string d2;
  for (int i = 0; i < 1500; ++i) {
    std::this_thread::sleep_for(1ms);
    d2 = headerValue(rawGet(), http::Date);
    if (d2 != d1 && !d2.empty()) {
      break;
    }
  }
  ASSERT_NE(d1, d2) << "Date header did not change across boundary after waiting";
}

namespace {

struct ErrorCase {
  const char* name{};
  const char* request{};
  const char* expectedStatus{};  // substring (e.g. "400", "505")
};

class HttpErrorParamTest : public ::testing::TestWithParam<ErrorCase> {};

}  // namespace

TEST_P(HttpErrorParamTest, EmitsExpectedStatus) {
  ts.resetConfigAndPostUpdate([]([[maybe_unused]] HttpServerConfig& cfg) {});
  ts.router().setDefault([](const HttpRequestView& req) { return req.makeResponse(http::StatusCodeOK); });
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
  ts.router().setDefault([](const HttpRequestView& req) { return req.makeResponse("ok"); });
  // HTTP/1.0 without Connection: keep-alive should close
  test::ClientConnection clientConnection(port);
  NativeHandle fd = clientConnection.fd();
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
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("ok"); });
  test::ClientConnection clientConnection(port);
  NativeHandle fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string_view req = "GET /h HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  test::sendAll(fd, req);
  std::string first = test::recvWithTimeout(fd);
  ASSERT_TRUE(first.contains(MakeHttp1HeaderLine(http::Connection, http::keepalive)));
  std::string_view req2 = "GET /h2 HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  test::sendAll(fd, req2);
  std::string second = test::recvWithTimeout(fd);
  ASSERT_TRUE(second.contains(MakeHttp1HeaderLine(http::Connection, http::keepalive)));
}

namespace {
std::string sendRaw(std::string_view raw) {
  test::ClientConnection clientConnection(port);
  NativeHandle fd = clientConnection.fd();
  test::sendAll(fd, raw);
  std::string resp = test::recvWithTimeout(fd, 300ms);
  // server may close depending on error severity
  return resp;
}
}  // anonymous namespace

TEST(HttpMalformed, MissingSpacesInRequestLine) {
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK); });
  std::string resp = sendRaw("GET/abcHTTP/1.1\r\nHost: x\r\n\r\n");
  ASSERT_TRUE(resp.contains("400")) << resp;
}

TEST(HttpMalformed, OversizedHeaders) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxHeaderBytes(128); });
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK); });

  std::string big(200, 'A');
  std::string raw = "GET / HTTP/1.1\r\nHost: x\r\nX-Big: " + big + "\r\n\r\n";
  std::string resp = sendRaw(raw);
  ASSERT_TRUE(resp.contains("431")) << resp;
}

TEST(HttpMalformed, BadChunkExtensionHex) {
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK); });

  // Transfer-Encoding with invalid hex char 'Z'
  std::string raw = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\n";  // incomplete + invalid
  std::string resp = sendRaw(raw);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 400"));
}

TEST(HttpMethodParsing, AcceptsCaseInsensitiveMethodTokens) {
  // Ensure the server accepts method tokens in mixed case (robustness per RFC 9110 §2.5).
  ts.router().setDefault([](const HttpRequestView& req) {
    HttpResponse resp;
    // Echo the canonical method name (parser maps mixed-case to enum).
    resp.body(std::string("method=") + std::string(http::MethodToStr(req.method())));
    return resp;
  });

  // Representative variants for common methods.
  static constexpr std::pair<std::string_view, std::string_view> cases[] = {
      {"GET /ci HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", "GET"},
      {"get /ci HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", "GET"},
      {"GeT /ci HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", "GET"},
      {"POST /ci HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", "POST"},
      {"pOsT /ci HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", "POST"},
  };

  for (const auto& pair : cases) {
    std::string resp = sendRaw(pair.first);
    // Response should be 200 and include the method echoed in the body.
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << "Resp=" << resp;
    EXPECT_TRUE(resp.contains(std::string("method=") + std::string(pair.second))) << "Resp=" << resp;
  }
}

#ifdef AERONET_POSIX
TEST(HttpServerTelemetry, DogStatsDClientSendsMetricsWithServiceName) {
  test::UnixDogstatsdSink sink;

  TelemetryConfig tcfg;
  tcfg.withDogStatsdSocketPath(sink.path()).withDogStatsdNamespace("svc").enableDogStatsDMetrics(true);

  HttpServerConfig cfg;
  cfg.withTelemetryConfig(std::move(tcfg));

  SingleHttpServer server(cfg);

  tracing::TelemetryContext telemetryContext(cfg.telemetry);

  auto* pDogStatsDClient = telemetryContext.dogstatsdClient();
  ASSERT_NE(pDogStatsDClient, nullptr);

  auto& dogStatsDClient = *pDogStatsDClient;

  dogStatsDClient.increment("metric1", 2);
  telemetryContext.counterAdd("metric2", 1);
  EXPECT_EQ(sink.recvMessage(), "svc.metric1:2|c");
  EXPECT_EQ(sink.recvMessage(), "svc.metric2:1|c");

  dogStatsDClient.gauge("gauge1", 2);
  telemetryContext.gauge("gauge2", 3);
  EXPECT_EQ(sink.recvMessage(), "svc.gauge1:2|g");
  EXPECT_EQ(sink.recvMessage(), "svc.gauge2:3|g");
}

TEST(HttpServerTelemetry, DogStatsDClientSendsMetricsWithoutServiceName) {
  test::UnixDogstatsdSink sink;

  TelemetryConfig tcfg;
  tcfg.withDogStatsdSocketPath(sink.path()).enableDogStatsDMetrics(true);
  tcfg.withServiceName("tel");

  HttpServerConfig cfg;
  cfg.withTelemetryConfig(std::move(tcfg));

  SingleHttpServer server(cfg);

  tracing::TelemetryContext telemetryContext(cfg.telemetry);
  auto* pDogStatsDClient = telemetryContext.dogstatsdClient();
  ASSERT_NE(pDogStatsDClient, nullptr);

  auto& dogStatsDClient = *pDogStatsDClient;

  dogStatsDClient.increment("metric1", 2);
  EXPECT_EQ(sink.recvMessage(), "tel.metric1:2|c");
}
#endif

TEST(HttpUrlDecoding, SpaceDecoding) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/hello world", [](const HttpRequestView& req) {
    return HttpResponse(http::StatusCodeOK).reason("OK").body(std::string(req.path()));
  });
  test::RequestOptions optHello;
  optHello.method = "GET";
  optHello.target = "/hello%20world";
  auto respOwned = test::requestOrThrow(ts.server.port(), optHello);
  std::string_view resp = respOwned;
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200 OK"));
  EXPECT_TRUE(resp.ends_with("hello world"));
}

TEST(HttpUrlDecoding, Utf8Decoded) {
  // Path contains snowman + space + 'x'
  std::string decodedPath = "/\xE2\x98\x83 x";  // /☃ x
  ts.resetRouterAndGet().setPath(http::Method::GET, decodedPath,
                                 [](const HttpRequestView&) { return HttpResponse("utf8"); });
  // Percent-encoded UTF-8 for snowman (E2 98 83) plus %20 and 'x'
  test::RequestOptions optUtf8;
  optUtf8.method = "GET";
  optUtf8.target = "/%E2%98%83%20x";
  auto respOwned = test::requestOrThrow(ts.server.port(), optUtf8);
  std::string_view resp = respOwned;
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(resp.ends_with("utf8"));
}

TEST(HttpUrlDecoding, PlusIsNotSpace) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/a+b",
                                 [](const HttpRequestView&) { return HttpResponse("plus"); });
  test::RequestOptions optPlus;
  optPlus.method = "GET";
  optPlus.target = "/a+b";
  auto respOwned = test::requestOrThrow(ts.server.port(), optPlus);
  std::string_view resp = respOwned;
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(resp.ends_with("plus"));
}

TEST(HttpUrlDecoding, InvalidPercentSequence400) {
  test::RequestOptions optBad;
  optBad.method = "GET";
  optBad.target = "/bad%G1";
  auto respOwned = test::requestOrThrow(ts.server.port(), optBad);
  std::string_view resp = respOwned;
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 400"));
}

TEST(HttpUrlDecoding, IncompletePercentSequence400) {
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/bad%";
  auto resp = test::requestOrThrow(ts.server.port(), opt);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 400"));
}

TEST(HttpUrlDecoding, MixedSegmentsDecoding) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/seg one/part%/two",
                                 [](const HttpRequestView& req) { return HttpResponse(req.path()); });
  // encodes space in first segment only
  test::RequestOptions opt2;
  opt2.method = "GET";
  opt2.target = "/seg%20one/part%25/two";
  auto resp = test::requestOrThrow(ts.server.port(), opt2);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(resp.ends_with("/seg one/part%/two"));
}

// ============================
// Zerocopy mode integration tests
// ============================

TEST(ZerocopyMode, LargeResponseWithZerocopyOpportunistic) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withZerocopyMode(ZerocopyMode::Opportunistic); });

  // Create a payload larger than the zerocopy threshold (16KB)
  constexpr std::size_t kLargePayloadSize = 32UL * 1024;  // 32 KB
  const std::string largePayload(kLargePayloadSize, 'Z');

  ts.resetRouterAndGet().setPath(http::Method::GET, "/large", [&largePayload](const HttpRequestView& req) {
    return req.makeResponse(largePayload);
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/large";
  auto resp = test::requestOrThrow(ts.port(), opt);

  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(resp.ends_with(largePayload));
}

TEST(ZerocopyMode, LargeResponseWithZerocopyDisabled) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withZerocopyMode(ZerocopyMode::Disabled); });

  constexpr std::size_t kLargePayloadSize = 32UL * 1024;  // 32 KB
  const std::string largePayload(kLargePayloadSize, 'D');

  ts.resetRouterAndGet().setPath(http::Method::GET, "/large-disabled", [&largePayload](const HttpRequestView& req) {
    return req.makeResponse(largePayload);
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/large-disabled";
  auto resp = test::requestOrThrow(ts.port(), opt);

  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(resp.ends_with(largePayload));
}

TEST(ZerocopyMode, LargeResponseWithZerocopyEnabled) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withZerocopyMode(ZerocopyMode::Enabled); });

  constexpr std::size_t kLargePayloadSize = 32UL * 1024;  // 32 KB
  const std::string largePayload(kLargePayloadSize, 'E');

  ts.resetRouterAndGet().setPath(http::Method::GET, "/large-enabled", [&largePayload](const HttpRequestView& req) {
    return req.makeResponse(largePayload);
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/large-enabled";
  auto resp = test::requestOrThrow(ts.port(), opt);

  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(resp.ends_with(largePayload));
}

TEST(ZerocopyMode, SmallResponseDoesNotUseZerocopy) {
  // Small responses (< 16KB) should bypass zerocopy even when enabled
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withZerocopyMode(ZerocopyMode::Enabled); });

  const std::string smallPayload = "Small response body";

  ts.resetRouterAndGet().setPath(http::Method::GET, "/small", [&smallPayload](const HttpRequestView& req) {
    return req.makeResponse(smallPayload);
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/small";
  auto resp = test::requestOrThrow(ts.port(), opt);

  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(resp.ends_with(smallPayload));
}

// ============================
// Zerocopy stress tests — data integrity under backpressure
// ============================
#ifdef AERONET_LINUX
TEST(ZerocopyMode, ForcedModeSmallPayload) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(0);
  });

  const std::string payload = "ForcedZerocopySmall";

  ts.resetRouterAndGet().setPath(http::Method::GET, "/forced-small",
                                 [&payload](const HttpRequestView& req) { return req.makeResponse(payload); });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/forced-small";
  auto resp = test::requestOrThrow(ts.port(), opt);

  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(resp.ends_with(payload));
}

TEST(ZerocopyMode, StressLargePayloadDataIntegrity) {
  // Stress test: repeated requests with large payloads to exercise zerocopy + backpressure.
  // Reproduces data corruption seen under sustained zerocopy with virtual network devices (K8s).
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(0);
  });

  // 1 MB payload with deterministic pattern to verify data integrity
  constexpr std::size_t kPayloadSize = 1UL << 20;
  std::string largePayload;
  largePayload.reserve(kPayloadSize);
  for (std::size_t idx = 0; idx < kPayloadSize; ++idx) {
    largePayload.push_back(static_cast<char>('A' + (idx % 26)));
  }

  ts.resetRouterAndGet().setPath(http::Method::GET, "/stress", [&largePayload](const HttpRequestView& req) {
    return req.makeResponse(largePayload);
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/stress";
  opt.recvTimeout = std::chrono::milliseconds{5000};

  constexpr int kIterations = 50;
  for (int iter = 0; iter < kIterations; ++iter) {
    auto resp = test::requestOrThrow(ts.port(), opt);
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << "iteration " << iter;
    ASSERT_TRUE(resp.ends_with(largePayload)) << "data corruption at iteration " << iter;
  }
}

TEST(ZerocopyMode, StressConcurrentLargePayloads) {
  // Concurrent stress test: multiple threads making large payload requests simultaneously.
  // Uses Opportunistic mode which auto-disables MSG_ZEROCOPY on loopback connections.
  // This test verifies large-payload concurrency under the zerocopy-enabled configuration
  // (even though loopback falls back to regular write). Single-threaded zerocopy data integrity
  // is covered by StressLargePayloadDataIntegrity and StressVaryingPayloadSizes.
  // Note: Forced mode on loopback triggers a kernel-level data corruption (page-aligned 32KB
  // block shifts) under concurrent connections on Linux >= 6.x, which is not reproducible
  // on real NICs where MSG_ZEROCOPY is actually useful.
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withZerocopyMode(ZerocopyMode::Opportunistic); });

  constexpr std::size_t kPayloadSize = 512UL * 1024;  // 512 KB
  std::string largePayload;
  largePayload.reserve(kPayloadSize);
  for (std::size_t idx = 0; idx < kPayloadSize; ++idx) {
    largePayload.push_back(static_cast<char>('0' + (idx % 10)));
  }

  ts.resetRouterAndGet().setPath(http::Method::GET, "/concurrent-stress", [&largePayload](const HttpRequestView& req) {
    return req.makeResponse(largePayload);
  });

  constexpr int kThreads = 8;
  constexpr int kRequestsPerThread = 20;

  vector<std::thread> threads;
  std::atomic<int> failures{0};

  threads.reserve(kThreads);
  for (int th = 0; th < kThreads; ++th) {
    threads.emplace_back([&]() {
      test::RequestOptions opt;
      opt.method = "GET";
      opt.target = "/concurrent-stress";
      opt.recvTimeout = std::chrono::milliseconds{10000};

      for (int req = 0; req < kRequestsPerThread; ++req) {
        try {
          auto resp = test::requestOrThrow(ts.port(), opt);
          if (!resp.starts_with("HTTP/1.1 200") || !resp.ends_with(largePayload)) {
            ++failures;
          }
        } catch (...) {
          ++failures;
        }
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  EXPECT_EQ(failures.load(), 0) << "Some requests had data corruption or failures under concurrency";
}

TEST(ZerocopyMode, StressVaryingPayloadSizes) {
  // Stress test with varying payload sizes exercising both zerocopy and regular write paths.
  // Forces zerocopy even for small payloads to stress the zerocopy completion mechanism.
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(0);
  });

  // Payloads of different sizes to exercise edge cases
  static constexpr std::size_t sizes[] = {100, 1024, 8UL * 1024, 16UL * 1024, 64UL * 1024, 256UL * 1024, 1024UL * 1024};

  for (std::size_t sz : sizes) {
    std::string payload(sz, static_cast<char>('a' + (sz % 26)));
    const std::string path = "/vary-" + std::to_string(sz);

    ts.resetRouterAndGet().setPath(http::Method::GET, path,
                                   [payload](const HttpRequestView& req) { return req.makeResponse(payload); });

    test::RequestOptions opt;
    opt.method = "GET";
    opt.target = path;
    opt.recvTimeout = std::chrono::milliseconds{5000};

    constexpr int kRepeat = 10;
    for (int rp = 0; rp < kRepeat; ++rp) {
      auto resp = test::requestOrThrow(ts.port(), opt);
      ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << "size=" << sz << " rep=" << rp;
      ASSERT_TRUE(resp.ends_with(payload)) << "data corruption at size=" << sz << " rep=" << rp;
    }
  }
}

TEST(ZerocopyMode, StressKeepAliveBackpressure) {
  // Stress test over a single keep-alive connection with many large responses.
  // This exercises the flushOutbound / outBuffer append paths with zerocopy.
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(0);
    cfg.withKeepAliveMode(true);
    cfg.withMaxRequestsPerConnection(10000);
  });

  static constexpr std::size_t kPayloadSize = 256UL * 1024;  // 256 KB
  std::string payload;
  payload.reserve(kPayloadSize);
  for (std::size_t idx = 0; idx < kPayloadSize; ++idx) {
    payload.push_back(static_cast<char>('A' + (idx % 26)));
  }

  ts.resetRouterAndGet().setPath(http::Method::GET, "/ka-stress",
                                 [&payload](const HttpRequestView& req) { return req.makeResponse(payload); });

  // Send many requests over a single connection using keep-alive
  test::ClientConnection cnx(ts.port());
  const NativeHandle fd = cnx.fd();
  test::setRecvTimeout(fd, std::chrono::milliseconds{5000});

  constexpr int kRequests = 30;
  for (int iter = 0; iter < kRequests; ++iter) {
    const std::string reqStr = "GET /ka-stress HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
    test::sendAll(fd, reqStr);

    auto resp = test::recvWithTimeout(fd, std::chrono::milliseconds{5000},
                                      kPayloadSize + 119U);  // 119 = length of HTTP headers
    ASSERT_FALSE(resp.empty()) << "empty response at iteration " << iter;
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << "bad status at iteration " << iter;
    ASSERT_TRUE(resp.contains(payload)) << "data corruption at iteration " << iter;
  }
}
#endif

// Basic trailer parsing test
TEST(HttpTrailers, BasicTrailer) {
  ts.router().setDefault([](const HttpRequestView& req) {
    EXPECT_EQ(req.body(), "Wikipedia");
    // Check trailer headers
    EXPECT_EQ(req.trailers().size(), 1U);
    EXPECT_EQ(req.trailerValueOrEmpty("X-Checksum"), "abc123");
    EXPECT_EQ(req.trailerValueOrEmpty("missing"), "");
    EXPECT_EQ(req.trailerValue("X-Checksum").value_or(""), "abc123");
    EXPECT_FALSE(req.trailerValue("Non-Existent").has_value());

    EXPECT_TRUE(req.hasHeader("Host"));
    EXPECT_FALSE(req.hasHeader("Non-Existent"));

    EXPECT_TRUE(req.hasTrailer("X-Checksum"));
    EXPECT_FALSE(req.hasTrailer("Non-Existent"));

    auto it = req.trailers().find("X-Checksum");
    EXPECT_NE(it, req.trailers().end());
    if (it != req.trailers().end()) {
      EXPECT_EQ(it->second, "abc123");
    }
    return req.makeResponse("OK");
  });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "POST /trailer HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "4\r\nWiki\r\n"
      "5\r\npedia\r\n"
      "0\r\n"
      "X-Checksum: abc123\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

// Multiple trailer headers
TEST(HttpTrailers, MultipleTrailers) {
  ts.router().setDefault([](const HttpRequestView& req) {
    EXPECT_EQ(req.body(), "test");
    EXPECT_EQ(req.trailers().size(), 3U);

    auto checksum = req.trailers().find("X-Checksum");
    EXPECT_NE(checksum, req.trailers().end());
    if (checksum != req.trailers().end()) {
      EXPECT_EQ(checksum->second, "xyz789");
    }

    auto timestamp = req.trailers().find("X-Timestamp");
    EXPECT_NE(timestamp, req.trailers().end());
    if (timestamp != req.trailers().end()) {
      EXPECT_EQ(timestamp->second, "2025-10-20T12:00:00Z");
    }

    auto custom = req.trailers().find("X-Custom-Trailer");
    EXPECT_NE(custom, req.trailers().end());
    if (custom != req.trailers().end()) {
      EXPECT_EQ(custom->second, "value123");
    }

    return req.makeResponse("OK");
  });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "POST /multi HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "X-Checksum: xyz789\r\n"
      "X-Timestamp: 2025-10-20T12:00:00Z\r\n"
      "X-Custom-Trailer: value123\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

// Regression: a chunked request carrying trailers followed by a fixed-length request on the SAME keep-alive
// connection must not leak the first request's trailers into the second. The per-connection trailerLen is
// reset for non-chunked bodies (see SingleHttpServer::decodeFixedLengthBody); without that reset the second
// request would spuriously re-parse the previous request's trailer bytes.
TEST(HttpTrailers, TrailersDoNotLeakAcrossKeepAliveRequests) {
  ts.router().setDefault(
      [](const HttpRequestView& req) { return req.makeResponse("trailers=" + std::to_string(req.trailers().size())); });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  // First request: chunked body + one trailer; keep the connection open (no Connection: close).
  const std::string chunkedWithTrailer =
      "POST /first HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ndata\r\n"
      "0\r\n"
      "X-Checksum: abc123\r\n"
      "\r\n";
  test::sendAll(fd, chunkedWithTrailer);
  const std::string first = test::recvWithTimeout(fd, 500ms);  // NOLINT(misc-include-cleaner)
  ASSERT_TRUE(first.starts_with("HTTP/1.1 200")) << first;
  EXPECT_TRUE(first.contains("trailers=1")) << first;

  // Second request on the same connection: fixed-length, no trailers.
  const std::string fixedLength =
      "POST /second HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Content-Length: 5\r\n"
      "Connection: close\r\n"
      "\r\n"
      "hello";
  test::sendAll(fd, fixedLength);
  const std::string second = test::recvUntilClosed(fd);
  ASSERT_TRUE(second.starts_with("HTTP/1.1 200")) << second;
  EXPECT_TRUE(second.contains("trailers=0")) << second;  // must NOT inherit the first request's trailer
}

// Empty trailers (just zero chunk and terminating CRLF)
TEST(HttpTrailers, NoTrailers) {
  ts.router().setDefault([](const HttpRequestView& req) {
    EXPECT_EQ(req.body(), "data");
    EXPECT_TRUE(req.trailers().empty());
    return req.makeResponse("OK");
  });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "POST /notrailer HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "4\r\ndata\r\n"
      "0\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

// Trailer with whitespace trimming
TEST(HttpTrailers, TrailerWhitespaceTrim) {
  ts.router().setDefault([](const HttpRequestView& req) {
    auto trailer = req.trailers().find("X-Data");
    EXPECT_NE(trailer, req.trailers().end());
    if (trailer != req.trailers().end()) {
      EXPECT_EQ(trailer->second, "trimmed");  // should be trimmed
    }
    return req.makeResponse("OK");
  });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "POST /trim HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "2\r\nhi\r\n"
      "0\r\n"
      "X-Data:   trimmed  \r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

// Forbidden trailer: Transfer-Encoding
TEST(HttpTrailers, ForbiddenTrailerTransferEncoding) {
  ts.router().setDefault([](const HttpRequestView& req) { return req.makeResponse("FAIL"); });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "POST /forbidden HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400"));
}

// Forbidden trailer: Content-Length
TEST(HttpTrailers, ForbiddenTrailerContentLength) {
  ts.router().setDefault([](const HttpRequestView& req) { return req.makeResponse("FAIL"); });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "POST /forbidden HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "Content-Length: 100\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400"));
}

// Forbidden trailer: Host
TEST(HttpTrailers, ForbiddenTrailerHost) {
  ts.router().setDefault([](const HttpRequestView& req) { return req.makeResponse("FAIL"); });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "POST /forbidden HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "Host: evil.com\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400"));
}

// Forbidden trailer: Authorization
TEST(HttpTrailers, ForbiddenTrailerAuthorization) {
  ts.resetRouterAndGet().setDefault([](const HttpRequestView& req) { return req.makeResponse("FAIL"); });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "POST /forbidden HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "Authorization: Bearer token123\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400"));
}

// Trailer size exceeds limit
TEST(HttpTrailers, TrailerSizeLimit) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withMaxHeaderBytes(200);  // match header limit
  });

  ts.resetRouterAndGet().setDefault([](const HttpRequestView& req) { return req.makeResponse("FAIL"); });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  // Create a very large trailer that exceeds the 200-byte limit
  std::string largeValue(300, 'X');
  std::string req =
      "POST /largetrailer HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "X-Large: " +
      largeValue +
      "\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 431"));
}

// Trailer with empty value
TEST(HttpTrailers, TrailerEmptyValue) {
  ts.router().setDefault([](const HttpRequestView& req) {
    auto trailer = req.trailers().find("X-Empty");
    EXPECT_NE(trailer, req.trailers().end());
    if (trailer != req.trailers().end()) {
      EXPECT_TRUE(trailer->second.empty());
    }
    return req.makeResponse("OK");
  });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "POST /empty HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "X-Empty:\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

// Case-insensitive trailer lookup
TEST(HttpTrailers, TrailerCaseInsensitive) {
  ts.router().setDefault([](const HttpRequestView& req) {
    // Should be able to find with different case
    auto lower = req.trailers().find("x-checksum");
    auto upper = req.trailers().find("X-CHECKSUM");
    auto mixed = req.trailers().find("X-Checksum");

    EXPECT_NE(lower, req.trailers().end());
    EXPECT_NE(upper, req.trailers().end());
    EXPECT_NE(mixed, req.trailers().end());

    if (lower != req.trailers().end()) {
      EXPECT_EQ(lower->second, "test123");
    }
    return HttpResponse("OK");
  });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "POST /case HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "X-Checksum: test123\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

// Duplicate trailers that should be merged using list semantics (comma)
TEST(HttpTrailers, DuplicateMergeTrailers) {
  ts.router().setDefault([](const HttpRequestView& req) {
    // Accept header should be merged with a comma separator
    auto it = req.trailers().find("Accept");
    EXPECT_NE(it, req.trailers().end());
    if (it != req.trailers().end()) {
      EXPECT_EQ(it->second, "text/html,application/json");
    }
    return HttpResponse("OK");
  });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "POST /dupmerge HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "Accept: text/html\r\n"
      "Accept: application/json\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

// Duplicate trailers with override semantics (keep last)
TEST(HttpTrailers, DuplicateOverrideTrailers) {
  ts.router().setDefault([](const HttpRequestView& req) {
    auto it = req.trailers().find("From");
    EXPECT_NE(it, req.trailers().end());
    if (it != req.trailers().end()) {
      // 'From' has override semantics in ReqHeaderValueSeparator, keep the last occurrence
      EXPECT_EQ(it->second, "b@example.com");
    }
    return HttpResponse("OK");
  });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "POST /dupoverride HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "From: a@example.com\r\n"
      "From: b@example.com\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

// Unknown header duplicates when mergeUnknownRequestHeaders is disabled -> should be rejected
TEST(HttpTrailers, UnknownHeaderNoMergeTrailers) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMergeUnknownRequestHeaders(false); });

  // "Handler should not be called when unknown-header duplicates are forbidden"
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("FAIL"); });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "POST /unknownnomerge HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "X-Experimental: a\r\n"
      "X-Experimental: b\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400"));
}

// Malformed trailer (no colon)
TEST(HttpTrailers, MalformedTrailerNoColon) {
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("FAIL"); });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "POST /malformed HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "4\r\ntest\r\n"
      "0\r\n"
      "MalformedTrailer\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 400"));
}

// Non-chunked request should have empty trailers
TEST(HttpTrailers, NonChunkedNoTrailers) {
  ts.router().setDefault([](const HttpRequestView& req) {
    EXPECT_EQ(req.body(), "test");
    EXPECT_TRUE(req.trailers().empty());
    return HttpResponse("OK");
  });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "POST /fixed HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Content-Length: 4\r\n"
      "Connection: close\r\n"
      "\r\n"
      "test";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

// Test streaming response with trailers
TEST(HttpResponseWriterTrailers, BasicStreamingTrailer) {
  ts.router().setPath(http::Method::GET, "/stream", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentType("text/custom");
    writer.writeBody("chunk1");
    writer.writeBody("chunk2");
    writer.trailerAddLine("x-checksum", "abc123");
    writer.end();
  });

  ts.router().setPath(http::Method::GET, "/normal", [](const HttpRequestView& req) {
    auto resp = req.makeResponse("chunk1chunk2", "text/custom");
    resp.trailerAddLine("x-checksum", "abc123");
    return resp;
  });

  for (bool withTrailerHeader : {true, false}) {
    ts.postConfigUpdate([withTrailerHeader](HttpServerConfig& cfg) { cfg.withTrailerHeader(withTrailerHeader); });

    for (std::string_view path : {"/stream", "/normal"}) {
      test::ClientConnection sock(port);
      NativeHandle fd = sock.fd();

      std::string req = "GET ";
      req += path;
      req +=
          " HTTP/1.1\r\n"
          "Host: example.com\r\n"
          "Connection: close\r\n"
          "\r\n";

      test::sendAll(fd, req);
      std::string resp = test::recvUntilClosed(fd);

      ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));

      // Check for chunked encoding
      EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentType, "text/custom")));
      EXPECT_FALSE(resp.contains(http::ContentLength));
      EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));

      // Check for Trailer header
      if (withTrailerHeader && path == "/normal") {
        // aeronet does not support adding Trailer header in HttpResponseWriter streaming mode for now.
        // It could in the future, if we propose a HttpResponseWriter::declareTrailers() method before headers are sent.
        EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine(http::Trailer, "x-checksum")));
      }

      // Check for chunks
      EXPECT_TRUE(resp.contains("chunk1"));
      EXPECT_TRUE(resp.contains("chunk2"));

      // Check for trailer (appears after the 0-size chunk)
      EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine("x-checksum", "abc123")));
    }
  }
}

// Test multiple trailers
TEST(HttpResponseWriterTrailers, MultipleTrailers) {
  ts.router().setDefault([](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(200);
    writer.writeBody("data");
    writer.trailerAddLine("X-Checksum", "xyz789");
    writer.trailerAddLine("X-Timestamp", "2025-10-20T12:00:00Z");
    writer.trailerAddLine("X-Custom", "value");
    writer.end();
  });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "GET /multi HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);

  EXPECT_TRUE(resp.contains("X-Checksum: xyz789"));
  EXPECT_TRUE(resp.contains("X-Timestamp: 2025-10-20T12:00:00Z"));
  EXPECT_TRUE(resp.contains("X-Custom: value"));
}

// Test trailer with empty value
TEST(HttpResponseWriterTrailers, EmptyValue) {
  ts.router().setDefault([](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("test");
    writer.trailerAddLine("X-Empty", "");
    writer.end();
  });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "GET /empty HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);

  // Empty value should still create the header line
  EXPECT_TRUE(resp.contains("X-Empty:"));
}

// Test trailer added after end() is ignored
TEST(HttpResponseWriterTrailers, AfterEndIgnored) {
  ts.router().setDefault([](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("test");
    writer.end();
    writer.trailerAddLine("X-Late", "ignored");  // Should be ignored
  });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "GET /late HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);

  // Late trailer should NOT appear
  EXPECT_FALSE(resp.contains("X-Late"));
}

// Test trailers ignored for fixed-length responses
TEST(HttpResponseWriterTrailers, IgnoredForFixedLength) {
  ts.router().setDefault([](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentLength(4);  // Fixed length
    writer.writeBody("test");
    writer.trailerAddLine("X-Ignored", "value");  // Should be ignored
    writer.end();
  });

  test::ClientConnection sock(port);
  NativeHandle fd = sock.fd();

  std::string req =
      "GET /fixed HTTP/1.1\r\n"
      "Host: example.com\r\n"
      "Connection: close\r\n"
      "\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);

  // Should use Content-Length, not chunked
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentLength, "4")));
  EXPECT_FALSE(resp.contains(http::TransferEncoding));

  // Trailer should NOT appear
  EXPECT_FALSE(resp.contains("X-Ignored"));
}

TEST(HttpStats, BasicCountersIncrement) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxRequestsPerConnection(5); });
  ts.router().setDefault(
      []([[maybe_unused]] const HttpRequestView& req) { return req.makeResponse(200).body("hello"); });
  // Single request via throwing helper
  auto resp = test::requestOrThrow(ts.port());
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  auto st = ts.server.stats();
  EXPECT_GT(st.totalBytesQueued, 0U);  // headers+body accounted
  EXPECT_GT(st.totalBytesWrittenImmediate + st.totalBytesWrittenFlush, 0U);
  EXPECT_GE(st.maxConnectionOutboundBuffer, 0U);
  EXPECT_GE(st.flushCycles, 0U);
}

// Test that ServerStats::json_str contains all numeric scalar fields and basic JSON structure without
// requiring brittle full-string matching. This makes the test resilient to new fields being added.
TEST(ServerStatsJson, ContainsAllScalarFields) {
  ServerStats st;
  // Populate with some non-zero, distinct-ish values so textual search is unique.
  st.totalBytesQueued = 42;
  st.totalBytesWrittenImmediate = 7;
  st.totalBytesWrittenFlush = 99;
  st.deferredWriteEvents = 3;
  st.flushCycles = 5;
  st.epollModFailures = 1;
  st.maxConnectionOutboundBuffer = 1234;
#ifdef AERONET_ENABLE_OPENSSL
  st.tlsHandshakesSucceeded = 2;
  st.tlsClientCertPresent = 0;
  st.tlsAlpnStrictMismatches = 0;
  st.tlsHandshakeDurationCount = 4;
  st.tlsHandshakeDurationTotalNs = 5555;
  st.tlsHandshakeDurationMaxNs = 999;
  st.tlsAlpnDistribution.emplace_back("http/1.1", 1);
  st.tlsVersionCounts.emplace_back("TLSv1.3", 2);
  st.tlsCipherCounts.emplace_back("TLS_AES_256_GCM_SHA384", 2);
#endif

  std::string json = st.json_str();
  ASSERT_FALSE(json.empty());
  ASSERT_EQ(json.front(), '{');
  ASSERT_EQ(json.back(), '}');

  // Collect expected scalar fields & verify presence of "name":value pattern.
  st.for_each_field([&](const char* name, uint64_t value) {
    std::string needle = std::string("\"") + name + "\":" + std::to_string(value);
    EXPECT_TRUE(json.contains(needle)) << "Missing field mapping: " << needle << " in json=" << json;
  });

  // Minimal structural sanity: no trailing comma before closing brace.
  ASSERT_FALSE(json.contains(",}")) << "Trailing comma present in JSON: " << json;
}

TEST(HttpOptionsTrace, OptionsStarReturnsAllow) {
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse(200); });

  auto resp =
      test::requestOrThrow(port, test::RequestOptions{.method = "OPTIONS", .target = "*", .body = "", .headers = {}});
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  std::string allow(http::Allow);
  allow += http::HeaderSep;
  ASSERT_TRUE(resp.contains(allow));
}

TEST(HttpOptionsTrace, TraceEchoWhenEnabled) {
  ts.postConfigUpdate(
      [](HttpServerConfig& cfg) { cfg.withTracePolicy(HttpServerConfig::TraceMethodPolicy::EnabledPlainAndTLS); });

  auto resp = test::requestOrThrow(
      port,
      test::RequestOptions{.method = "TRACE", .target = "/test", .body = "", .headers = {{"X-Test-Header", "value"}}});

  ASSERT_FALSE(resp.empty());

  // TRACE response must be message/http
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentType, "message/http")));

  // Should echo request line
  ASSERT_TRUE(resp.contains("TRACE /test HTTP/"));

  // Should echo headers
  ASSERT_TRUE(resp.contains("X-Test-Header: value"));
}

TEST(HttpOptionsTrace, TraceDisabledReturns405) {
  ts.postConfigUpdate(
      [](HttpServerConfig& cfg) { cfg.withTracePolicy(HttpServerConfig::TraceMethodPolicy::Disabled); });

  auto resp =
      test::requestOrThrow(port, test::RequestOptions{.method = "TRACE", .target = "/test", .body = "", .headers = {}});
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 405"));
}

TEST(HttpOptionsTrace, TraceEnabledPlainOnlyAllowsPlaintext) {
  ts.postConfigUpdate(
      [](HttpServerConfig& cfg) { cfg.withTracePolicy(HttpServerConfig::TraceMethodPolicy::EnabledPlainOnly); });

  // Send TRACE over plaintext
  auto resp =
      test::requestOrThrow(port, test::RequestOptions{.method = "TRACE", .target = "/test", .body = "", .headers = {}});
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

namespace {
CorsPolicy MakePolicy() {
  CorsPolicy policy;
  policy.allowOrigin("https://app.example")
      .allowMethods(http::Method::GET | http::Method::POST)
      .allowAnyRequestHeaders();
  return policy;
}

class HttpCorsIntegration : public ::testing::Test {
 protected:
  static RouterConfig MakeConfigWithCors() {
    RouterConfig cfg{};
    cfg.withDefaultCorsPolicy(MakePolicy());
    return cfg;
  }

  void SetUp() override { ts.router() = Router{MakeConfigWithCors()}; }
};
}  // namespace

TEST_F(HttpCorsIntegration, PreflightUsesRouterAllowedMethods) {
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequestView&) { return HttpResponse("ok"); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {
      {"Origin", "https://app.example"},
      {"Access-Control-Request-Method", "GET"},
      {"Access-Control-Request-Headers", "X-Trace"},
  };

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeNoContent);

  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://app.example");

  auto methodsIt = parsed.headers.find(http::AccessControlAllowMethods);
  ASSERT_NE(methodsIt, parsed.headers.end());
  EXPECT_EQ(methodsIt->second, "GET");

  auto hdrsIt = parsed.headers.find(http::AccessControlAllowHeaders);
  ASSERT_NE(hdrsIt, parsed.headers.end());
  EXPECT_EQ(hdrsIt->second, "*");
}

TEST_F(HttpCorsIntegration, PreflightMethodDeniedReturns405WithAllow) {
  ts.router().setPath(http::Method::GET, "/data",
                      [](const HttpRequestView& req) { return req.makeResponse(http::StatusCodeOK); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}, {"Access-Control-Request-Method", "PUT"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeMethodNotAllowed);

  auto allowIt = parsed.headers.find(http::Allow);
  ASSERT_NE(allowIt, parsed.headers.end());
  EXPECT_EQ(allowIt->second, "GET");
}

TEST_F(HttpCorsIntegration, PreflightOriginDeniedReturns403) {
  ts.router().setPath(http::Method::GET, "/data",
                      [](const HttpRequestView& req) { return req.makeResponse(http::StatusCodeOK); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://denied.example"}, {"Access-Control-Request-Method", "GET"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeForbidden);

  EXPECT_FALSE(parsed.headers.contains(http::AccessControlAllowOrigin));
}

TEST_F(HttpCorsIntegration, ActualRequestIncludesAllowOriginHeader) {
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequestView&) { return HttpResponse("ok"); });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://app.example");
}

TEST_F(HttpCorsIntegration, ActualRequestOriginDeniedReturns403) {
  std::atomic<bool> handlerInvoked{false};
  ts.router().setPath(http::Method::GET, "/data", [&handlerInvoked](const HttpRequestView&) {
    handlerInvoked.store(true);
    return HttpResponse(http::StatusCodeOK);
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://blocked.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeForbidden);
  EXPECT_FALSE(handlerInvoked.load());
}

TEST_F(HttpCorsIntegration, StreamingResponseCarriesCorsHeaders) {
  ts.router().setPath(http::Method::GET, "/stream", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody("chunk-one");
    writer.end();
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/stream";
  opt.headers = {{"Origin", "https://app.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://app.example");

  // Verify Vary: Origin is present for mirrored origin (credentials enabled in fixture)
  auto varyIt = parsed.headers.find(http::Vary);
  ASSERT_NE(varyIt, parsed.headers.end());
  EXPECT_TRUE(varyIt->second.contains(http::Origin));

  EXPECT_EQ(parsed.plainBody, "chunk-one");
}

TEST_F(HttpCorsIntegration, StreamingVaryHeaderAppendsOrigin) {
  ts.router().setPath(http::Method::GET, "/stream", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.header(http::Vary, http::AcceptEncoding);
    writer.contentType("text/plain");
    writer.writeBody("data");
    writer.end();
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/stream";
  opt.headers = {{"Origin", "https://app.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

  auto varyIt = parsed.headers.find(http::Vary);
  ASSERT_NE(varyIt, parsed.headers.end());
  EXPECT_TRUE(varyIt->second.contains(http::AcceptEncoding));
  EXPECT_TRUE(varyIt->second.contains(http::Origin));
}

TEST_F(HttpCorsIntegration, StreamingOriginDeniedSkipsHandler) {
  std::atomic<bool> handlerInvoked{false};
  ts.router().setPath(http::Method::GET, "/stream",
                      [&handlerInvoked](const HttpRequestView&, HttpResponseWriter& writer) {
                        handlerInvoked.store(true);
                        writer.status(http::StatusCodeOK);
                        writer.writeBody("should-not-send");
                        writer.end();
                      });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/stream";
  opt.headers = {{"Origin", "https://blocked.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeForbidden);
  EXPECT_FALSE(handlerInvoked.load());
  EXPECT_EQ(parsed.headers.count(http::AccessControlAllowOrigin), 0);
}

TEST_F(HttpCorsIntegration, PerRouteCorsPolicyOverridesDefault_ActualAndPreflight) {
  // Attach a per-route policy that only allows https://per.example and GET
  CorsPolicy per;
  per.allowOrigin("https://per.example").allowMethods(http::Method::GET).allowAnyRequestHeaders();

  ts.router()
      .setPath(http::Method::GET, "/per", [](const HttpRequestView&) { return HttpResponse("ok"); })
      .cors(std::move(per));

  // Actual request with allowed per-route origin
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/per";
  opt.headers = {{"Origin", "https://per.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://per.example");

  // Actual request with origin allowed by router default but not per-route -> should be denied
  opt.headers = {{"Origin", "https://app.example"}};
  raw = test::requestOrThrow(ts.port(), opt);
  parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeForbidden);

  // Preflight for per-route allowed origin
  test::RequestOptions pre;
  pre.method = "OPTIONS";
  pre.target = "/per";
  pre.headers = {{"Origin", "https://per.example"}, {"Access-Control-Request-Method", "GET"}};

  raw = test::requestOrThrow(ts.port(), pre);
  parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeNoContent);
  originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://per.example");
}

TEST(HttpCorsDetailed, PreflightWithCredentialsEmitsMirroredOriginAndCredentials) {
  CorsPolicy policy = MakePolicy();
  policy.allowCredentials(true);
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};

  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequestView&) { return HttpResponse("ok"); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}, {"Access-Control-Request-Method", "GET"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeNoContent);

  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://app.example");

  auto credIt = parsed.headers.find(http::AccessControlAllowCredentials);
  ASSERT_NE(credIt, parsed.headers.end());
  EXPECT_EQ(credIt->second, "true");
}

TEST(HttpCorsDetailed, ActualRequestWithCredentialsEmitsCredentials) {
  CorsPolicy policy = MakePolicy();
  policy.allowCredentials(true);
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequestView&) { return HttpResponse("ok"); });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://app.example");

  auto credIt = parsed.headers.find(http::AccessControlAllowCredentials);
  ASSERT_NE(credIt, parsed.headers.end());
  EXPECT_EQ(credIt->second, "true");
}

TEST(HttpCorsDetailed, PreflightExposeHeadersAndMaxAge) {
  CorsPolicy policy = MakePolicy();
  policy.exposeHeader("X-My-Header").maxAge(std::chrono::seconds{600});
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequestView&) { return HttpResponse("ok"); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}, {"Access-Control-Request-Method", "GET"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeNoContent);

  auto exposIt = parsed.headers.find(http::AccessControlExposeHeaders);
  ASSERT_NE(exposIt, parsed.headers.end());
  EXPECT_EQ(exposIt->second, "X-My-Header");

  auto maxAgeIt = parsed.headers.find(http::AccessControlMaxAge);
  ASSERT_NE(maxAgeIt, parsed.headers.end());
  EXPECT_EQ(maxAgeIt->second, "600");
}

TEST(HttpCorsDetailed, PreflightPrivateNetworkHeader) {
  CorsPolicy policy = MakePolicy();
  policy.allowPrivateNetwork(true);
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequestView&) { return HttpResponse("ok"); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}, {"Access-Control-Request-Method", "GET"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeNoContent);

  auto pnetIt = parsed.headers.find(http::AccessControlAllowPrivateNetwork);
  ASSERT_NE(pnetIt, parsed.headers.end());
  EXPECT_EQ(pnetIt->second, "true");
}

TEST(HttpCorsDetailed, PreflightRequestedHeaderDeniedWhenNotAllowed) {
  CorsPolicy policy;
  policy.allowOrigin("https://app.example");
  policy.allowMethods(http::Method::GET | http::Method::POST);
  policy.allowRequestHeader("X-Foo");
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::GET, "/data",
                      [](const HttpRequestView& req) { return req.makeResponse(http::StatusCodeOK); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {
      {"Origin", "https://app.example"},
      {"Access-Control-Request-Method", "GET"},
      {"Access-Control-Request-Headers", "X-Bar"},
  };

  auto raw = test::requestOrThrow(port, opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeForbidden);
  EXPECT_EQ(parsed.headers.count(http::AccessControlAllowHeaders), 0);
}

TEST(HttpCorsDetailed, PreflightEchoesRequestedHeadersWhenNoAllowedList) {
  CorsPolicy policy;
  policy.allowOrigin("https://app.example");
  policy.allowMethods(http::Method::GET | http::Method::POST);
  // Do not call allowAnyRequestHeaders or allowRequestHeader -> allowedRequestHeaders empty
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::GET, "/data",
                      [](const HttpRequestView& req) { return req.makeResponse(http::StatusCodeOK); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {
      {"Origin", "https://app.example"},
      {"Access-Control-Request-Method", "GET"},
      {"Access-Control-Request-Headers", "  X-Trace , X-Other  "},
  };

  auto raw = test::requestOrThrow(port, opt);
  auto parsed = test::parseResponseOrThrow(raw);
  // When no allowed-request-headers are configured and we did not call allowAnyRequestHeaders(),
  // a non-empty requested header list should be denied (HeadersDenied -> 403).
  EXPECT_EQ(parsed.statusCode, http::StatusCodeForbidden);
  EXPECT_EQ(parsed.headers.count(http::AccessControlAllowHeaders), 0);
}

TEST(HttpCorsDetailed, VaryIncludesOriginWhenMirroring) {
  // Case 1: no existing Vary -> should add 'Origin'
  {
    CorsPolicy policy;
    policy.allowOrigin("https://app.example").allowCredentials(true);
    RouterConfig routerCfg;
    routerCfg.withDefaultCorsPolicy(std::move(policy));

    ts.router() = Router{routerCfg};
    ts.router().setPath(http::Method::GET, "/data", [](const HttpRequestView&) { return HttpResponse("ok"); });

    test::RequestOptions opt;
    opt.method = "GET";
    opt.target = "/data";
    opt.headers = {{"Origin", "https://app.example"}};

    auto raw = test::requestOrThrow(ts.port(), opt);
    auto parsed = test::parseResponseOrThrow(raw);
    EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

    auto varyIt = parsed.headers.find(http::Vary);
    ASSERT_NE(varyIt, parsed.headers.end());
    EXPECT_TRUE(varyIt->second.contains(http::Origin));
  }

  // Case 2: existing Vary -> should append ', Origin'
  {
    CorsPolicy policy;
    policy.allowOrigin("https://app.example").allowCredentials(true);
    RouterConfig routerCfg;
    routerCfg.withDefaultCorsPolicy(std::move(policy));

    ts.router() = Router{routerCfg};
    ts.router().setPath(http::Method::GET, "/data", [](const HttpRequestView&) {
      HttpResponse resp(http::StatusCodeOK);
      resp.header(http::Vary, http::AcceptEncoding);
      return resp;
    });

    test::RequestOptions opt;
    opt.method = "GET";
    opt.target = "/data";
    opt.headers = {{"Origin", "https://app.example"}};

    auto raw = test::requestOrThrow(ts.port(), opt);
    auto parsed = test::parseResponseOrThrow(raw);
    EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

    auto varyIt = parsed.headers.find(http::Vary);
    ASSERT_NE(varyIt, parsed.headers.end());
    EXPECT_TRUE(varyIt->second.contains(http::AcceptEncoding));
    EXPECT_TRUE(varyIt->second.contains(http::Origin));
  }
}

TEST(HttpCorsDetailed, VaryNoDuplicateWhenOriginAlreadyPresent) {
  CorsPolicy policy;
  policy.allowOrigin("https://app.example").allowCredentials(true);
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequestView&) {
    HttpResponse resp(http::StatusCodeOK);
    resp.headerAddLine(http::Vary, http::Origin);
    return resp;
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

  auto varyIt = parsed.headers.find(http::Vary);
  ASSERT_NE(varyIt, parsed.headers.end());
  EXPECT_TRUE(varyIt->second.contains(http::Origin));
  std::string expectedEndOrigin = ", ";
  expectedEndOrigin += http::Origin;
  EXPECT_FALSE(varyIt->second.contains(expectedEndOrigin));
}

TEST(HttpCorsDetailed, MultipleAllowedOriginsMirrorCorrectOne) {
  CorsPolicy policy;
  policy.allowOrigin("https://one.example");
  policy.allowOrigin("https://two.example");
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::GET, "/data", [](const HttpRequestView&) { return HttpResponse("ok"); });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://two.example"}};

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);

  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://two.example");
}

TEST(HttpCorsDetailed, OptionsWithoutAcrMethodTreatedAsSimpleCors) {
  CorsPolicy policy;
  policy.allowOrigin("https://app.example").allowMethods(static_cast<http::MethodBmp>(http::Method::GET));
  RouterConfig routerCfg;
  routerCfg.withDefaultCorsPolicy(std::move(policy));

  ts.router() = Router{routerCfg};
  ts.router().setPath(http::Method::OPTIONS, "/data",
                      [](const HttpRequestView&) { return HttpResponse(http::StatusCodeNoContent); });

  test::RequestOptions opt;
  opt.method = "OPTIONS";
  opt.target = "/data";
  opt.headers = {{"Origin", "https://app.example"}};  // no Access-Control-Request-Method header

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeNoContent);
  auto originIt = parsed.headers.find(http::AccessControlAllowOrigin);
  ASSERT_NE(originIt, parsed.headers.end());
  EXPECT_EQ(originIt->second, "https://app.example");
}

TEST(HttpsRedirectIntegration, BasicGetRedirectStandardPort) {
  ts.resetConfigAndPostUpdate(
      [](HttpServerConfig& cfg) { cfg.withHttpsRedirect(443, http::StatusCodeMovedPermanently); });
  // A handler that must NOT be reached (redirect bypasses routing).
  ts.router().setPath(http::Method::GET, "/path",
                      [](const HttpRequestView& req) { return req.makeResponse(http::StatusCodeOK, "handler"); });

  test::RequestOptions opt;
  opt.target = "/path";
  opt.host = "example.com:80";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(port, opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_EQ(test::getHeader(resp, "location"), "https://example.com/path");
  EXPECT_EQ(test::toLower(test::getHeader(resp, "connection")), "close");
}

TEST(HttpsRedirectIntegration, NonStandardPortAppended) {
  ts.resetConfigAndPostUpdate(
      [](HttpServerConfig& cfg) { cfg.withHttpsRedirect(8443, http::StatusCodeMovedPermanently); });

  test::RequestOptions opt;
  opt.target = "/secure";
  opt.host = "example.com";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(port, opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_EQ(test::getHeader(resp, "location"), "https://example.com:8443/secure");
}

TEST(HttpsRedirectIntegration, PreservesQueryString) {
  ts.resetConfigAndPostUpdate(
      [](HttpServerConfig& cfg) { cfg.withHttpsRedirect(443, http::StatusCodeMovedPermanently); });

  test::RequestOptions opt;
  opt.target = "/search?q=foo&lang=en";
  opt.host = "example.com";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(port, opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_EQ(test::getHeader(resp, "location"), "https://example.com/search?q=foo&lang=en");
}

TEST(HttpsRedirectIntegration, CustomStatusCode308) {
  ts.resetConfigAndPostUpdate(
      [](HttpServerConfig& cfg) { cfg.withHttpsRedirect(443, http::StatusCodePermanentRedirect); });
  test::RequestOptions opt;
  opt.target = "/";
  opt.host = "host.test";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(port, opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodePermanentRedirect);
  EXPECT_EQ(test::getHeader(resp, "location"), "https://host.test/");
}

TEST(HttpsRedirectIntegration, PostWithBodyIsRedirected) {
  ts.resetConfigAndPostUpdate(
      [](HttpServerConfig& cfg) { cfg.withHttpsRedirect(443, http::StatusCodeMovedPermanently); });

  test::RequestOptions opt;
  opt.method = "POST";
  opt.target = "/submit";
  opt.host = "example.com";
  opt.body = "payload=1234";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(port, opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_EQ(test::getHeader(resp, "location"), "https://example.com/submit");
}

TEST(HttpsRedirectIntegration, MissingHostReturns400) {
  ts.resetConfigAndPostUpdate(
      [](HttpServerConfig& cfg) { cfg.withHttpsRedirect(443, http::StatusCodeMovedPermanently); });

  // HTTP/1.0 request without a Host header: no absolute https URL can be built -> 400.
  const std::string raw = "GET /x HTTP/1.0\r\n\r\n";
  auto resp = test::parseResponseOrThrow(test::sendAndCollect(port, raw));

  EXPECT_EQ(resp.statusCode, http::StatusCodeBadRequest);
}

TEST(HttpsRedirectIntegration, IPv6HostPreservedWithBrackets) {
  ts.resetConfigAndPostUpdate(
      [](HttpServerConfig& cfg) { cfg.withHttpsRedirect(8443, http::StatusCodeMovedPermanently); });

  test::RequestOptions opt;
  opt.target = "/a";
  opt.host = "[::1]:80";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(port, opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_EQ(test::getHeader(resp, "location"), "https://[::1]:8443/a");
}

TEST(HttpsRedirectIntegration, HeadRequestRedirectedWithoutBody) {
  ts.resetConfigAndPostUpdate(
      [](HttpServerConfig& cfg) { cfg.withHttpsRedirect(443, http::StatusCodeMovedPermanently); });
  test::RequestOptions opt;
  opt.method = "HEAD";
  opt.target = "/x";
  opt.host = "example.com";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(port, opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_EQ(test::getHeader(resp, "location"), "https://example.com/x");
  EXPECT_TRUE(resp.body.empty());
}

TEST(HttpsRedirectIntegration, EmitsRequestMetrics) {
  ts.resetConfigAndPostUpdate(
      [](HttpServerConfig& cfg) { cfg.withHttpsRedirect(443, http::StatusCodeMovedPermanently); });
  std::atomic<http::StatusCode> seenStatus{0};
  std::atomic<int> count{0};
  ts.server.setMetricsCallback([&](const RequestMetrics& requestMetrics) {
    seenStatus.store(requestMetrics.status);
    count.fetch_add(1);
  });

  test::RequestOptions opt;
  opt.target = "/metrics-path";
  opt.host = "example.com";
  auto resp = test::parseResponseOrThrow(test::requestOrThrow(port, opt));

  EXPECT_EQ(resp.statusCode, http::StatusCodeMovedPermanently);
  EXPECT_EQ(count.load(), 1);
  EXPECT_EQ(seenStatus.load(), http::StatusCodeMovedPermanently);

  ts.server.setMetricsCallback({});  // reset callback
}

namespace {
std::string BlockingFetch(std::string_view verb, std::string_view target) {
  test::RequestOptions opt;
  opt.method = verb;
  opt.target = target;
  opt.connection = "close";  // one-shot
  auto resp = test::request(port, opt);
  if (!resp) {
    return {};
  }
  return *resp;
}

std::string RequestVerb(std::string_view verb, std::string_view target) {
  test::ClientConnection sock(port);
  auto fd = sock.fd();

  std::string req(verb);
  req.push_back(' ');
  req.append(target);

  req.append(" HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  test::sendAll(fd, req);
  return test::recvUntilClosed(fd);
}

std::string RequestMethod(std::string_view method, std::string_view path, std::string_view body = {}) {
  test::ClientConnection cnx(port);
  auto fd = cnx.fd();

  std::string req = std::string(method) + " " + std::string(path) + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n";
  if (!body.empty()) {
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  req += "\r\n";
  req += body;

  test::sendAll(fd, req);
  return test::recvUntilClosed(fd);
}

// Very small chunked decoder for tests (single pass, no trailers). Expects full HTTP response.
std::string ExtractBody(std::string_view resp) {
  auto headerEnd = resp.find(http::DoubleCRLF);
  if (headerEnd == std::string::npos) {
    return {};
  }
  std::string body(resp.substr(headerEnd + http::DoubleCRLF.size()));
  // If not chunked just return remaining.
  if (!body.contains("\r\n0\r\n") && !body.contains(http::EndChunk)) {
    return body;
  }  // heuristic
  std::string out;
  std::size_t pos = 0;
  while (pos < body.size()) {
    auto lineEnd = body.find("\r\n", pos);
    if (lineEnd == std::string::npos) {
      break;
    }
    std::string sizeHex = body.substr(pos, lineEnd - pos);
    std::size_t sz = 0;
    try {
      sz = static_cast<std::size_t>(std::stoul(sizeHex, nullptr, 16));
    } catch (...) {
      break;
    }
    pos = lineEnd + 2;
    if (sz == 0) {
      break;
    }
    if (pos + sz > body.size()) {
      break;
    }
    out.append(body, pos, sz);
    pos += sz + 2;  // skip data + CRLF
  }
  return out;
}

}  // namespace

TEST(HttpStreaming, ChunkedSimple) {
  ts.resetConfig();
  ts.resetRouterAndGet().setDefault([]([[maybe_unused]] const HttpRequestView& req, HttpResponseWriter& writer) {
    writer.status(200);
    writer.contentType("text/plain");
    EXPECT_THROW(writer.header("Invalid Header", "value"), std::invalid_argument);
    EXPECT_THROW(writer.headerAddLine("Invalid Header", "value"), std::invalid_argument);
    EXPECT_THROW(writer.headerAddLine("X-Header", "value\r\n"), std::invalid_argument);
    writer.headerAddLine("X-Custom", "value");
    writer.contentType("text/custom");  // should be allowed to update content type before body is written
    writer.writeBody("hello ");
    writer.status(400);                             // should be ignored after headers sent
    writer.headerAddLine("X-Custom-2", "value 2");  // should be ignored after headers sent
    writer.writeBody("world");
    writer.end();
    writer.end();  // second end() should be no-op
  });
  std::string resp = BlockingFetch("GET", "/stream");
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("X-Custom: value\r\n"));
  ASSERT_FALSE(resp.contains("X-Custom-2"));  // header added after headers sent should be ignored
  // Should contain chunk sizes in hex (6 and 5) and terminating 0 chunk.
  ASSERT_TRUE(resp.contains("6\r\nhello "));
  ASSERT_TRUE(resp.contains("5\r\nworld"));
  ASSERT_TRUE(resp.contains(http::EndChunk));
}

TEST(HttpStreaming, HttpHeaderValuesAreTrimmed) {
  ts.resetConfig();
  ts.router().setDefault([]([[maybe_unused]] const HttpRequestView& req, HttpResponseWriter& writer) {
    writer.status(200);
    writer.header("X-Trimmed", "   trimmed-value   ");
    writer.headerAddLine("X-Also-Trimmed", "  another-trim  ");
    writer.writeBody("data");
    writer.end();
  });
  std::string resp = BlockingFetch("GET", "/trim-headers");
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(resp.contains("X-Trimmed: trimmed-value\r\n"));
  EXPECT_TRUE(resp.contains("X-Also-Trimmed: another-trim\r\n"));
}

TEST(HttpStreaming, SendFileFixedLengthPlain) {
  constexpr std::string_view kPayload = "static sendfile response body";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);

  std::string path = tmp.filePath().string();

  ts.router().setDefault([path](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(200);
    writer.file(File(path));
    writer.end();
    writer.status(404);          // should be ignored after end
    writer.reason("Not Found");  // should be ignored after end
  });

  std::string resp = BlockingFetch("GET", "/file");

  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  ASSERT_FALSE(resp.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentLength, std::to_string(kPayload.size()))));

  auto headerEnd = resp.find(http::DoubleCRLF);
  ASSERT_NE(std::string::npos, headerEnd);
  std::string body = resp.substr(headerEnd + http::DoubleCRLF.size());
  EXPECT_EQ(body, kPayload);
}

TEST(HttpStreaming, WriteBodyAndTrailersShouldFailIfSendFileIsUsed) {
  constexpr std::string_view kPayload = "file body";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);

  std::string path = tmp.filePath().string();

  ts.router().setDefault([path](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(200);
    writer.file(File(path));
    EXPECT_FALSE(writer.writeBody("extra data"));  // should be no-op
    writer.trailerAddLine("X-Trailer", "value");   // should be no-op
    writer.end();
  });

  std::string resp = BlockingFetch("GET", "/file");

  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  ASSERT_FALSE(resp.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentLength, std::to_string(kPayload.size()))));

  auto headerEnd = resp.find(http::DoubleCRLF);
  ASSERT_NE(std::string::npos, headerEnd);
  std::string body = resp.substr(headerEnd + http::DoubleCRLF.size());
  EXPECT_EQ(body, kPayload);  // extra data should not appear
}

TEST(HttpStreaming, SendFileHeadSuppressesBody) {
  constexpr std::string_view kPayload = "head sendfile streaming";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);

  std::string path = tmp.filePath().string();

  ts.router().setDefault([path](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(200);
    writer.file(File(path));
    writer.end();
  });

  std::string resp = BlockingFetch("HEAD", "/file");

  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentLength, std::to_string(kPayload.size()))));
  ASSERT_FALSE(resp.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));

  auto headerEnd = resp.find(http::DoubleCRLF);
  ASSERT_NE(std::string::npos, headerEnd);
  std::string body = resp.substr(headerEnd + http::DoubleCRLF.size());
  EXPECT_TRUE(body.empty());
}

TEST(HttpStreaming, SendFileErrors) {
  ts.router().setDefault([]([[maybe_unused]] const HttpRequestView& req, HttpResponseWriter& writer) {
    writer.status(200);
    EXPECT_TRUE(writer.writeBody("initial data"));
    EXPECT_FALSE(writer.file(File("/nonexistent/path")));  // should be no-op
    writer.end();
    EXPECT_FALSE(writer.file(File("/nonexistent/path")));  // should be no-op
  });

  std::string resp = BlockingFetch("GET", "/file-after-write");

  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));

  EXPECT_TRUE(resp.contains(http::DoubleCRLF));
  EXPECT_EQ(ExtractBody(resp), "initial data") << resp;
}

TEST(HttpStreaming, SendFileOverrideContentLength) {
  constexpr std::string_view kPayload = "file with overridden content length";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);

  std::string path = tmp.filePath().string();

  ts.router().setDefault([path](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(200);
    writer.contentLength(10);  // will be ignored (warning log only)
    writer.file(File(path));
    writer.end();
  });

  std::string resp = BlockingFetch("GET", "/file-override-cl");

  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentLength, "35")));
  ASSERT_FALSE(resp.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));

  auto headerEnd = resp.find(http::DoubleCRLF);
  ASSERT_NE(std::string::npos, headerEnd);
  std::string body = resp.substr(headerEnd + http::DoubleCRLF.size());
  EXPECT_EQ(body.size(), 35);
  EXPECT_EQ(body, kPayload);
}

TEST(HttpStreaming, HeadSuppressedBody) {
  ts.router().setDefault([]([[maybe_unused]] const HttpRequestView& req, HttpResponseWriter& writer) {
    writer.status(200);
    writer.contentType("text/plain");
    writer.writeBody("ignored body");  // should not be emitted for HEAD
    writer.end();
  });
  std::string resp = BlockingFetch("HEAD", "/head");
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  // For HEAD we expect no chunked framing. "0\r\n" alone would falsely match the Content-Length header line
  // ("Content-Length: 0\r\n"). What we really want to assert is that there is no terminating chunk sequence.
  // The terminating chunk in a chunked response would appear as "\r\n0\r\n\r\n" (preceded by the blank line
  // after headers or previous chunk). We also assert absence of Transfer-Encoding: chunked and body payload.
  ASSERT_FALSE(resp.contains("\r\n0\r\n\r\n"));
  ASSERT_FALSE(resp.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));
  ASSERT_FALSE(resp.contains("ignored body"));
  // Positive check: we do expect a Content-Length: 0 header for HEAD.
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentLength, "0")));
}

#ifdef AERONET_ENABLE_ZLIB
TEST(HttpStreamingCompression, StreamingWriterAppendsVaryAcceptEncoding) {
  CompressionConfig compression;
  compression.minBytes = 8;
  compression.preferredFormats.clear();
  compression.preferredFormats.push_back(Encoding::gzip);
  compression.addVaryAcceptEncodingHeader = true;

  ts.postConfigUpdate([compression](HttpServerConfig& serverCfg) { serverCfg.withCompression(compression); });

  ts.router().setPath(http::Method::GET, "/vary-writer", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.header(http::Vary, http::Origin);
    writer.contentType("text/plain");
    writer.writeBody(std::string(64, 'a'));
    writer.end();
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/vary-writer";
  opt.headers = {{"Accept-Encoding", "gzip"}};

  const auto raw = test::requestOrThrow(port, opt);
  const auto parsed = test::parseResponseOrThrow(raw);

  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  auto varyIt = parsed.headers.find(http::Vary);
  ASSERT_NE(varyIt, parsed.headers.end());
  EXPECT_TRUE(varyIt->second.contains(http::Origin));
  EXPECT_TRUE(varyIt->second.contains(http::AcceptEncoding));

  ts.postConfigUpdate([compression](HttpServerConfig& serverCfg) { serverCfg.withCompression({}); });
}

TEST(HttpStreamingCompression, AddHeaderContentEncodingIdentityShouldNotAutomaticallyCompress) {
  CompressionConfig compression;
  compression.minBytes = 8;
  compression.preferredFormats = {Encoding::gzip};
  compression.addVaryAcceptEncodingHeader = false;

  ts.postConfigUpdate([compression](HttpServerConfig& serverCfg) { serverCfg.withCompression(compression); });

  ts.router().setPath(http::Method::GET, "/identity-no-compress",
                      [](const HttpRequestView&, HttpResponseWriter& writer) {
                        writer.status(http::StatusCodeOK);
                        writer.contentEncoding("identity");
                        writer.contentType("text/plain");
                        writer.writeBody(std::string(64, 'a'));
                        writer.end();
                      });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/identity-no-compress";
  opt.headers = {{"Accept-Encoding", "gzip"}};

  const auto raw = test::requestOrThrow(port, opt);
  const auto parsed = test::parseResponseOrThrow(raw);

  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  auto ceIt = parsed.headers.find(http::ContentEncoding);
  ASSERT_NE(ceIt, parsed.headers.end());
  EXPECT_EQ(ceIt->second, "identity");
  EXPECT_EQ(parsed.body, std::string(64, 'a'));

  ts.postConfigUpdate([compression](HttpServerConfig& serverCfg) { serverCfg.withCompression({}); });
}

TEST(HttpStreamingCompression, MultiChunkCompressedWriteReusesBuffer) {
  CompressionConfig compression;
  compression.minBytes = 8;
  compression.preferredFormats = {Encoding::gzip};
  compression.addVaryAcceptEncodingHeader = false;

  ts.postConfigUpdate([compression](HttpServerConfig& serverCfg) { serverCfg.withCompression(compression); });

  // Build large unique chunks so the encoder produces output for intermediate writes,
  // exercising the _compressedBuffer reuse path in writeBody().
  constexpr std::size_t kChunkSize = static_cast<std::size_t>(256 * 1024);
  constexpr int kNumChunks = 4;
  std::string chunk(kChunkSize, '\0');
  for (std::size_t idx = 0; idx < kChunkSize; ++idx) {
    chunk[idx] = static_cast<char>('A' + (idx % 26));
  }
  std::string expected;
  expected.reserve(kChunkSize * kNumChunks);
  for (int ii = 0; ii < kNumChunks; ++ii) {
    expected += chunk;
  }

  ts.router().setPath(http::Method::GET, "/multi-chunk-compress",
                      [&chunk](const HttpRequestView&, HttpResponseWriter& writer) {
                        writer.status(http::StatusCodeOK);
                        writer.contentType("text/plain");
                        for (int ii = 0; ii < kNumChunks; ++ii) {
                          EXPECT_TRUE(writer.writeBody(chunk));
                        }
                        writer.end();
                      });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/multi-chunk-compress";
  opt.headers = {{"Accept-Encoding", "gzip"}};
  opt.maxResponseBytes = 2 << 20;

  const auto raw = test::requestOrThrow(port, opt);
  const auto parsed = test::parseResponseOrThrow(raw);

  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  auto ceIt = parsed.headers.find(http::ContentEncoding);
  ASSERT_NE(ceIt, parsed.headers.end());
  EXPECT_EQ(ceIt->second, "gzip");

  // Body must be smaller than identity (compression effective on repeated pattern)
  EXPECT_LT(parsed.body.size(), expected.size());

  // Round-trip decompress and verify correctness
  auto decompressed = test::Decompress(Encoding::gzip, parsed.body);
  EXPECT_EQ(std::string_view(decompressed.data(), decompressed.size()), expected);

  ts.postConfigUpdate([](HttpServerConfig& serverCfg) { serverCfg.withCompression({}); });
}
#endif

TEST(HttpStreamingSetHeader, MultipleCustomHeadersAndOverrideContentType) {
  ts.router().setDefault([]([[maybe_unused]] const HttpRequestView& req, HttpResponseWriter& writer) {
    writer.status(200);
    writer.header("X-Custom-A", "alpha");
    writer.header("X-Custom-B", "beta");
    writer.contentType("application/json");  // override default
    // First write sends headers implicitly.
    writer.writeBody("{\"k\":1}");
    // These should be ignored because headers already sent.
    writer.header("X-Ignored", "zzz");
    writer.contentType("text/plain");
    writer.end();
  });

  std::string getResp = RequestVerb("GET", "/hdr");
  std::string headResp = RequestVerb("HEAD", "/hdr");

  // Basic status line check
  ASSERT_TRUE(getResp.starts_with("HTTP/1.1 200"));
  ASSERT_TRUE(headResp.starts_with("HTTP/1.1 200"));
  // Custom headers should appear exactly once each.
  ASSERT_TRUE(getResp.contains("X-Custom-A: alpha\r\n"));
  ASSERT_TRUE(getResp.contains("X-Custom-B: beta\r\n"));
  // Overridden content type
  ASSERT_TRUE(getResp.contains(MakeHttp1HeaderLine(http::ContentType, "application/json")));
  // Default text/plain should not appear.
  ASSERT_FALSE(getResp.contains(MakeHttp1HeaderLine(http::ContentType, "text/plain")));
  // Ignored header should not appear.
  ASSERT_FALSE(getResp.contains("X-Ignored: zzz"));
  // Body present in GET but not in HEAD.
  ASSERT_TRUE(getResp.contains("{\"k\":1}"));
  ASSERT_FALSE(headResp.contains("{\"k\":1}"));
  // HEAD: ensure Content-Length auto added (0 since body suppressed) and no chunk framing.
  ASSERT_TRUE(headResp.contains(MakeHttp1HeaderLine(http::ContentLength, "0")));
  ASSERT_FALSE(headResp.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));
}

TEST(HttpServerMixed, MixedPerPathHandlers) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withKeepAliveMode(false); });

  // path /mix : GET streaming, POST normal
  ts.router().setPath(http::Method::GET, "/mix", [](const HttpRequestView& /*unused*/, HttpResponseWriter& writer) {
    writer.status(200);
    writer.contentType("text/plain");
    writer.writeBody("S");
    writer.writeBody("TREAM");
    writer.end();
  });
  ts.router().setPath(http::Method::POST, "/mix", [](const HttpRequestView& /*unused*/) {
    return HttpResponse(201).reason("Created").body("NORMAL");
  });
  std::string getResp = RequestMethod("GET", "/mix");
  auto decoded = ExtractBody(getResp);
  EXPECT_EQ(decoded, "STREAM");
  std::string postResp = RequestMethod("POST", "/mix", "x");
  EXPECT_TRUE(postResp.contains("NORMAL"));
}

TEST(HttpServerMixed, ConflictRegistrationNormalThenStreaming) {
  ts.router().setPath(http::Method::GET, "/c", [](const HttpRequestView&) { return HttpResponse("X"); });
  EXPECT_THROW(ts.router().setPath(http::Method::GET, "/c", [](const HttpRequestView&, HttpResponseWriter&) {}),
               std::logic_error);
}

TEST(HttpServerMixed, ConflictRegistrationStreamingThenNormal) {
  ts.router().setPath(http::Method::GET, "/c2", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(200);
    writer.end();
  });
  EXPECT_THROW(ts.router().setPath(http::Method::GET, "/c2", [](const HttpRequestView&) { return HttpResponse("Y"); }),
               std::logic_error);
}

TEST(HttpServerMixed, GlobalFallbackPrecedence) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.enableKeepAlive = false; });
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("GLOBAL"); });
  ts.router().setDefault([](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(200);
    writer.contentType("text/plain");
    writer.writeBody("STREAMFALLBACK");
    writer.end();
  });
  // path-specific streaming overrides both
  ts.router().setPath(http::Method::GET, "/s", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(200);
    writer.writeBody("PS");
    writer.end();
  });
  // path-specific normal overrides global fallbacks
  ts.router().setPath(http::Method::GET, "/n", [](const HttpRequestView&) { return HttpResponse("PN"); });

  std::string pathStreamResp = RequestMethod("GET", "/s");
  EXPECT_TRUE(pathStreamResp.contains("PS"));
  std::string pathNormalResp = RequestMethod("GET", "/n");
  EXPECT_TRUE(pathNormalResp.contains("PN"));
  std::string fallback = RequestMethod("GET", "/other");
  // Should use global streaming first (higher precedence than global normal)
  EXPECT_TRUE(fallback.contains("STREAMFALLBACK"));
}

TEST(HttpServerMixed, GlobalNormalOnlyWhenNoStreaming) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.enableKeepAlive = false; });
  ts.router().setDefault([](const HttpRequestView&) { return HttpResponse("GN"); });

  std::string result = RequestMethod("GET", "/x");
  EXPECT_TRUE(result.contains("GN"));
}

TEST(HttpServerMixed, HeadRequestOnStreamingPathSuppressesBody) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.enableKeepAlive = false; });
  // Register streaming handler for GET; it will attempt to write a body.
  ts.router().setPath(http::Method::GET, "/head", [](const HttpRequestView& /*unused*/, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody("SHOULD_NOT_APPEAR");  // for HEAD this must be suppressed by writer
    writer.end();
  });
  std::string headResp = RequestMethod("HEAD", "/head");
  // Body should be empty; ensure word not present and Content-Length: 0 (or if chunked not used at all)
  auto headerEnd = headResp.find(http::DoubleCRLF);
  ASSERT_NE(std::string::npos, headerEnd);
  std::string bodyPart = headResp.substr(headerEnd + http::DoubleCRLF.size());
  EXPECT_TRUE(bodyPart.empty());
  // Either explicit Content-Length: 0 is present or (future) alternate header; assert current behavior.
  EXPECT_TRUE(headResp.contains(MakeHttp1HeaderLine(http::ContentLength, "0")));
  EXPECT_FALSE(headResp.contains("SHOULD_NOT_APPEAR"));
}

TEST(HttpServerMixed, MethodNotAllowedWhenOnlyOtherStreamingMethodRegistered) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.enableKeepAlive = false; });
  // Register only GET streaming handler
  ts.router().setPath(http::Method::GET, "/m405", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("OKGET");
    writer.end();
  });
  std::string postResp = RequestMethod("POST", "/m405", "data");
  // Expect 405 Method Not Allowed
  EXPECT_TRUE(postResp.starts_with("HTTP/1.1 405"));
  EXPECT_TRUE(postResp.contains("Method Not Allowed"));
  // Ensure GET still works and returns streaming body
  std::string getResp2 = RequestMethod("GET", "/m405");
  auto decoded2 = ExtractBody(getResp2);
  EXPECT_EQ(decoded2, "OKGET");
}

TEST(HttpServerMixed, KeepAliveSequentialMixedStreamingAndNormal) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.enableKeepAlive = true;
    cfg.maxRequestsPerConnection = 3;  // allow at least two
  });
  // Register streaming GET and normal POST on same path
  ts.router().setPath(http::Method::GET, "/ka", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody("A");
    writer.writeBody("B");
    writer.end();
  });
  ts.router().setPath(http::Method::POST, "/ka",
                      [](const HttpRequestView&) { return HttpResponse(201).reason("Created").body("NORMAL"); });

  // Build raw requests (each must include Host and Connection: keep-alive)
  std::string r1 = "GET /ka HTTP/1.1\r\nHost: test\r\nConnection: keep-alive\r\n\r\n";  // streaming
  std::string r2 =
      "POST /ka HTTP/1.1\r\nHost: test\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";  // normal, closes

  test::ClientConnection cnx(port);

  test::sendAll(cnx.fd(), r1 + r2);

  std::string raw = test::recvUntilClosed(cnx.fd());

  // Should contain two HTTP/1.1 status lines, first 200 OK, second 201 Created
  auto firstPos = raw.find("HTTP/1.1 200");
  auto secondPos = raw.find("201 Created");
  EXPECT_NE(std::string::npos, firstPos);
  EXPECT_NE(std::string::npos, secondPos);
  // Decode first body (chunked) expecting AB
  auto firstHeaderEnd = raw.find(http::DoubleCRLF);
  ASSERT_NE(std::string::npos, firstHeaderEnd);
  auto afterFirst = raw.find("HTTP/1.1 201 Created", firstHeaderEnd);
  ASSERT_NE(std::string::npos, afterFirst);
  std::string firstResponse = raw.substr(0, afterFirst);
  auto body1 = ExtractBody(firstResponse);
  EXPECT_EQ(body1, "AB");
  // Second response should have NORMAL
  auto secondBodyStart = raw.find("NORMAL", afterFirst);
  EXPECT_NE(std::string::npos, secondBodyStart);
}

TEST(StreamingKeepAlive, TwoSequentialRequests) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.enableKeepAlive = true; });
  ts.router().setDefault([](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("hello");
    writer.writeBody(",world");
    writer.end();
  });

  test::ClientConnection cnx(port);
  auto fd = cnx.fd();
  std::string req1 = "GET / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  test::sendAll(fd, req1);
  auto r1 = test::recvWithTimeout(fd);
  ASSERT_FALSE(r1.empty());
  // Send second request on same connection.
  std::string req2 = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";  // request close after second
  test::sendAll(fd, req2);
  auto r2 = test::recvWithTimeout(fd);
  ASSERT_FALSE(r2.empty());
}

TEST(StreamingKeepAlive, HeadRequestReuse) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.enableKeepAlive = true; });
  ts.router().setDefault([](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("ignored-body");
    writer.end();
  });

  test::ClientConnection cnx(port);
  auto fd = cnx.fd();

  std::string hreq = "HEAD / HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  test::sendAll(fd, hreq);
  auto hr = test::recvWithTimeout(fd);
  // Ensure no body appears after header terminator.
  auto pos = hr.find(http::DoubleCRLF);
  ASSERT_NE(pos, std::string::npos);
  ASSERT_TRUE(hr.substr(pos + http::DoubleCRLF.size()).empty());
  // second GET
  std::string g2 = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, g2);
  auto gr2 = test::recvWithTimeout(fd);
  ASSERT_TRUE(gr2.contains("ignored-body"));  // ensure body from second request present
}

namespace {
void raw(auto port, std::string_view verb, std::string& out) {
  test::ClientConnection sock(port);
  auto fd = sock.fd();
  std::string req(verb);
  req += " /len HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  out = test::recvUntilClosed(fd);
}

void rawWith(auto port, std::string_view verb, std::string_view extraHeaders, std::string& out) {
  test::ClientConnection sock(port);
  auto fd = sock.fd();
  std::string req(verb);
  req += " /len HTTP/1.1\r\nHost: x\r\n";
  req += extraHeaders;
  req += "Connection: close\r\n\r\n";
  test::sendAll(fd, req);
  out = test::recvUntilClosed(fd);
}
}  // namespace

TEST(HttpStreamingHeadContentLength, HeadSuppressesBodyKeepsCL) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.enableKeepAlive = true;
    cfg.maxRequestsPerConnection = 2;
  });
  ts.router().setDefault([]([[maybe_unused]] const HttpRequestView& req, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    // We set Content-Length even though we write body pieces; for HEAD the body must be suppressed but CL retained.
    static constexpr std::string_view body = "abcdef";  // length 6
    writer.contentLength(body.size());
    writer.writeBody(body.substr(0, 3));
    writer.writeBody(body.substr(3));
    writer.end();
  });
  std::string headResp;
  std::string getResp;
  raw(port, "HEAD", headResp);
  raw(port, "GET", getResp);

  ASSERT_TRUE(headResp.starts_with("HTTP/1.1 200"));
  ASSERT_TRUE(headResp.contains(MakeHttp1HeaderLine(http::ContentLength, "6")));
  // No chunked framing, no body.
  ASSERT_FALSE(headResp.contains("abcdef"));
  ASSERT_FALSE(headResp.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));
  // GET path: should carry body; since we set fixed length it should not be chunked.
  ASSERT_TRUE(getResp.starts_with("HTTP/1.1 200"));
  ASSERT_TRUE(getResp.contains(MakeHttp1HeaderLine(http::ContentLength, "6")));
  ASSERT_TRUE(getResp.contains("abcdef"));
  ASSERT_FALSE(getResp.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));
}

TEST(HttpStreaming, ContentLengthAfterFirstWriteShouldBeIgnored) {
  ts.router().setDefault([](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("hello ");
    // This should be ignored (already wrote body bytes).
    writer.contentLength(9999);
    writer.writeBody("world");
    writer.end();
    writer.writeBody(" additional");  // should be no-op after end
  });
  std::string getResp;
  raw(port, "GET", getResp);
  ASSERT_TRUE(getResp.starts_with("HTTP/1.1 200"));
  // Should be chunked since we wrote body before setting length.
  ASSERT_TRUE(getResp.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));
  // Ensure our ignored length did not appear.
  ASSERT_FALSE(getResp.contains(MakeHttp1HeaderLine(http::ContentLength, "9999")));
  ASSERT_TRUE(getResp.contains("hello "));
  ASSERT_TRUE(getResp.contains("world"));
  ASSERT_FALSE(getResp.contains("additional"));  // after end
}

TEST(HttpStreamingHeadContentLength, StreamingNoContentLengthUsesChunked) {
  ts.router().setDefault([](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("abc");
    writer.writeBody("def");
    writer.writeBody("");  // empty body piece
    writer.end();
  });
  std::string getResp;
  raw(port, "GET", getResp);
  ASSERT_TRUE(getResp.starts_with("HTTP/1.1 200"));
  // No explicit Content-Length, chunked framing present.
  ASSERT_TRUE(getResp.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));
  ASSERT_FALSE(getResp.contains(http::ContentLength));
  ASSERT_TRUE(getResp.contains("abc"));
  ASSERT_TRUE(getResp.contains("def"));
}

TEST(HttpStreamingHeadContentLength, StreamingLateContentLengthIgnoredStaysChunked) {
  ts.router().setDefault([](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("part1");
    // This should be ignored (already wrote body bytes) and we remain in chunked mode.
    writer.contentLength(9999);
    writer.writeBody("part2");
    writer.end();
  });
  std::string getResp;
  raw(port, "GET", getResp);
  ASSERT_TRUE(getResp.starts_with("HTTP/1.1 200"));
  ASSERT_TRUE(getResp.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));
  // Ensure our ignored length did not appear.
  ASSERT_FALSE(getResp.contains(MakeHttp1HeaderLine(http::ContentLength, "9999")));
  ASSERT_TRUE(getResp.contains("part1"));
  ASSERT_TRUE(getResp.contains("part2"));
}

#ifdef AERONET_ENABLE_ZLIB
TEST(HttpStreamingHeadContentLength, StreamingContentLengthWithAutoCompressionDiscouragedButHonored) {
  // We intentionally (mis)use contentLength with auto compression; library will not adjust size.
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    CompressionConfig compCfg;
    compCfg.minBytes = 1;
    cfg.withCompression(std::move(compCfg));
  });
  static constexpr std::string_view kBody =
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";  // 64 'A'
  const std::size_t originalSize = kBody.size();
  ts.router().setDefault([&](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentLength(originalSize);  // declares uncompressed length
    writer.writeBody(kBody.substr(0, 10));
    writer.writeBody(kBody.substr(10));
    writer.end();
  });
  std::string resp;
  rawWith(port, "GET", "Accept-Encoding: gzip\r\n", resp);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  // We expect a fixed-length header present.
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentLength, std::to_string(originalSize))));
  // Compression should have activated producing a gzip header (1F 8B in hex) and Content-Encoding header.
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentEncoding, "gzip")));
  // Body should not be chunked.
  ASSERT_FALSE(resp.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));
  // Extract body (after double CRLF) and verify it differs from original (compressed) and starts with gzip magic.
  auto pos = resp.find(http::DoubleCRLF);
  ASSERT_NE(pos, std::string::npos);
  auto body = resp.substr(pos + 4);
  ASSERT_FALSE(body.empty());
  ASSERT_NE(body.find(std::string(kBody)), 0U) << "Body unexpectedly identical (compression not applied)";
  ASSERT_GE(body.size(), 2U);
  unsigned char b0 = static_cast<unsigned char>(body[0]);
  unsigned char b1 = static_cast<unsigned char>(body[1]);
  ASSERT_EQ(b0, 0x1f);  // gzip magic
  ASSERT_EQ(b1, 0x8b);
}
#endif

TEST(StreamingBackpressure, LargeBodyQueues) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.enableKeepAlive = false;                                       // simplicity
    cfg.maxOutboundBufferBytes = static_cast<std::size_t>(64 * 1024);  // assume default maybe larger
  });
  std::size_t total = static_cast<std::size_t>(512 * 1024);  // 512 KB
  ts.router().setDefault([&]([[maybe_unused]] const HttpRequestView& req, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    std::string chunk(8192, 'x');
    std::size_t sent = 0;
    while (sent < total) {
      writer.writeBody(chunk);
      sent += chunk.size();
    }
    writer.end();
  });
  test::ClientConnection cnx(port);
  auto fd = cnx.fd();
  std::string_view req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
  test::sendAll(fd, req);

  auto data = test::recvUntilClosed(fd);
  EXPECT_TRUE(data.starts_with("HTTP/1.1 200"));
}

TEST(HttpStreamingAdaptive, CoalescedAndLargePaths) {
  constexpr std::size_t kLargeSize = 5000;

  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMinCapturedBodySize(kLargeSize - 1U); });

  std::string large(kLargeSize, 'x');
  static constexpr std::byte kSmall[] = {
      std::byte{'s'}, std::byte{'m'}, std::byte{'a'}, std::byte{'l'}, std::byte{'l'},
  };
  ts.router().setDefault([&](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody(kSmall);  // coalesced path
    writer.writeBody(large);   // large path (multi enqueue)
    writer.end();
    EXPECT_TRUE(writer.finished());
    EXPECT_FALSE(writer.failed());
  });
  std::string resp = BlockingFetch("GET", "/adaptive");
  auto stats = ts.server.stats();
  EXPECT_GT(stats.totalBytesWrittenImmediate, kLargeSize);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200"));
  // Validate both chunk headers present: 5 and hex(kLargeSize)
  char hexBuf[32];
  auto res = std::to_chars(hexBuf, hexBuf + sizeof(hexBuf), static_cast<unsigned long long>(kLargeSize), 16);
  ASSERT_TRUE(res.ec == std::errc());
  std::string largeHex(hexBuf, res.ptr);
  ASSERT_TRUE(resp.contains("5\r\nsmall"));
  ASSERT_TRUE(resp.contains(largeHex + "\r\n"));
  // Count 'x' occurrences only in the body (after header terminator) to avoid false positives in headers.
  auto hdrEnd = resp.find(http::DoubleCRLF);
  ASSERT_NE(std::string::npos, hdrEnd);
  std::string_view body(resp.data() + hdrEnd + http::DoubleCRLF.size(), resp.size() - hdrEnd - http::DoubleCRLF.size());
  // Body is chunked: <5 CRLF small CRLF> <hex CRLF largePayload CRLF> 0 CRLF CRLF.
  // We only count 'x' in the large payload; small chunk contains none.
  ASSERT_EQ(kLargeSize, static_cast<size_t>(std::count(body.begin(), body.end(), 'x')));
}

TEST(HttpStreaming, CustomContentTypeAndEncoding) {
  if constexpr (!zlibEnabled()) {
    GTEST_SKIP();
  }
  // Set up server with compression enabled; provide mixed-case Content-Type and Content-Encoding headers via writer.
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.compression.minBytes = 1;
    cfg.compression.preferredFormats.assign(1U, Encoding::gzip);
  });
  std::string payload(128, 'Z');
  ts.router().setDefault([payload](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(200);
    writer.contentType("text/xml");
    writer.contentEncoding("identity");  // should suppress auto compression
    writer.writeBody(payload.substr(0, 40));
    writer.writeBody(payload.substr(40));
    writer.end();
  });
  test::ClientConnection cc(port);
  auto fd = cc.fd();
  std::string req =
      "GET /h HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  // Ensure our original casing appears exactly and no differently cased duplicate exists.
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentType, "text/xml"))) << resp;
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentEncoding, "identity"))) << resp;
  // Body should be identity (contains long run of 'Z').
  EXPECT_TRUE(resp.contains(std::string(50, 'Z'))) << "Body appears compressed when it should not";
}

// Test headerAddLine with Content-Encoding - sets _contentEncodingHeaderPresent
TEST(HttpResponseWriterFailures, AddHeaderContentEncoding) {
  ts.router().setPath(http::Method::GET, "/content-encoding", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.headerAddLine(http::ContentEncoding, "gzip");
    writer.contentType("text/plain");
    writer.writeBody("test");
    writer.end();
  });

  const std::string response = test::simpleGet(port, "/content-encoding");
  EXPECT_TRUE(response.contains(MakeHttp1HeaderLine(http::ContentEncoding, "gzip")));
}

// Test contentLength called after writeBody - should log warning and ignore
TEST(HttpResponseWriterFailures, ContentLengthAfterWrite) {
  ts.router().setPath(http::Method::GET, "/len-after-write", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("first");
    writer.contentLength(100);  // Should be ignored with _bytesWritten > 0
    writer.end();
  });

  const std::string response = test::simpleGet(port, "/len-after-write");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
  EXPECT_TRUE(response.contains(MakeHttp1HeaderLine(http::TransferEncoding, "chunked")));
}

// Test file() called after writeBody - should fail
TEST(HttpResponseWriterFailures, FileAfterWriteBody) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "test-content");

  ts.router().setPath(http::Method::GET, "/file-after-write",
                      [path = tmp.filePath().string()](const HttpRequestView&, HttpResponseWriter& writer) {
                        writer.status(http::StatusCodeOK);
                        writer.writeBody("data");

                        // Try to use file after writeBody
                        bool fileResult = writer.file(File(path));
                        EXPECT_FALSE(fileResult);  // Should fail because bytes already written

                        writer.end();
                      });

  const std::string response = test::simpleGet(port, "/file-after-write");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
}

// Test writeBody/trailerAddLine/end after end() - State::Ended checks
TEST(HttpResponseWriterFailures, OperationsAfterEnd) {
  ts.router().setPath(http::Method::GET, "/after-end", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("data");
    writer.end();

    // These should all be ignored (State::Ended)
    EXPECT_FALSE(writer.writeBody("more"));
    writer.trailerAddLine("X-Ignored", "value");
    writer.end();  // Second end() should be harmless
  });

  const std::string response = test::simpleGet(port, "/after-end");
  EXPECT_TRUE(response.contains("data"));
  EXPECT_FALSE(response.contains("more"));
}

// Test header/status operations after headers sent
TEST(HttpResponseWriterFailures, ModifyAfterHeadersSent) {
  ts.router().setPath(http::Method::GET, "/modify-after-headers",
                      [](const HttpRequestView&, HttpResponseWriter& writer) {
                        writer.status(http::StatusCodeOK);
                        writer.header("X-Before", "value1");
                        writer.writeBody("chunk1");  // This sends headers

                        // These should be ignored (State::HeadersSent)
                        writer.status(http::StatusCodeNotFound);     // Ignored
                        writer.header("X-After", "value2");          // Ignored
                        writer.headerAddLine("X-After2", "value3");  // Ignored
                        writer.contentLength(50);                    // Ignored

                        writer.writeBody("chunk2");
                        writer.end();
                      });

  const std::string response = test::simpleGet(port, "/modify-after-headers");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(response.contains("X-Before: value1"));
  EXPECT_FALSE(response.contains("X-After"));
}

// Test trailerAddLine for fixed-length response (non-chunked) - should be ignored
TEST(HttpResponseWriterFailures, TrailerForFixedLength) {
  ts.router().setPath(http::Method::GET, "/trailer-fixed-len", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentLength(4);  // Fixed length = non-chunked

    // trailerAddLine should be ignored before writing body ...
    writer.trailerAddLine("X-Trailer", "ignored");

    writer.writeBody("test");

    // ... and because not chunked
    writer.trailerAddLine("X-Trailer", "ignored");
    writer.end();
  });

  const std::string response = test::simpleGet(port, "/trailer-fixed-len");
  auto parsed = test::parseResponseOrThrow(response);
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(response.ends_with("\r\n\r\ntest"));
  EXPECT_EQ(4, parsed.body.size());
}

// Test writeBody with sendfile active - should be ignored
TEST(HttpResponseWriterFailures, WriteBodyWithFileActive) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "file-data");

  ts.router().setPath(http::Method::GET, "/write-with-file",
                      [path = tmp.filePath().string()](const HttpRequestView&, HttpResponseWriter& writer) {
                        writer.status(http::StatusCodeOK);
                        writer.file(File(path));

                        // writeBody should be ignored when file is active
                        EXPECT_FALSE(writer.writeBody("extra-data"));

                        writer.end();
                      });

  const std::string response = test::simpleGet(port, "/write-with-file");
  EXPECT_TRUE(response.contains("file-data"));
  EXPECT_FALSE(response.contains("extra-data"));
}

// Test file() not in Opened state - should fail
TEST(HttpResponseWriterFailures, FileNotInOpenedState) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "file-content");

  ts.router().setPath(http::Method::GET, "/file-wrong-state",
                      [path = tmp.filePath().string()](const HttpRequestView&, HttpResponseWriter& writer) {
                        writer.status(http::StatusCodeOK);
                        writer.writeBody("data");  // Transitions to HeadersSent

                        // file() should fail - not in Opened state anymore
                        EXPECT_FALSE(writer.file(File(path)));

                        writer.end();
                      });

  const std::string response = test::simpleGet(port, "/file-wrong-state");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200"));
}

// Test file() overriding previously declared Content-Length - should warn
TEST(HttpResponseWriterFailures, FileOverridesContentLength) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "overridden-file-content");

  ts.router().setPath(http::Method::GET, "/file-override-length",
                      [path = tmp.filePath().string()](const HttpRequestView&, HttpResponseWriter& writer) {
                        writer.status(http::StatusCodeOK);
                        writer.contentLength(100);  // Declare a length first

                        // file() should override the previously set contentLength
                        EXPECT_TRUE(writer.file(File(path)));

                        writer.end();
                      });

  const std::string response = test::simpleGet(port, "/file-override-length");
  EXPECT_TRUE(response.contains("overridden-file-content"));
}

// Test empty writeBody - should return true immediately
TEST(HttpResponseWriterFailures, WriteBodyEmpty) {
  ts.router().setPath(http::Method::GET, "/write-empty", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);

    // Empty writes should succeed immediately
    EXPECT_TRUE(writer.writeBody(""));
    EXPECT_TRUE(writer.writeBody(std::string_view{}));

    writer.writeBody("actual-data");
    writer.end();
  });

  const std::string response = test::simpleGet(port, "/write-empty");
  EXPECT_TRUE(response.contains("actual-data"));
}

// Test HEAD request - body should be suppressed
TEST(HttpResponseWriterFailures, HeadRequestSuppressesBody) {
  ts.router().setPath(http::Method::GET, "/head-test", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentType("text/plain");
    writer.writeBody("this-should-not-appear-in-head");
    writer.end();
  });

  test::RequestOptions opts;
  opts.method = "HEAD";
  opts.target = "/head-test";
  const std::string response = test::sendAndCollect(port, test::buildRequest(opts));
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(test::noBodyAfterHeaders(response));  // HEAD responses have no body
}

// Test multiple status() calls - last one wins before headers sent
TEST(HttpResponseWriterFailures, MultipleStatusCalls) {
  ts.router().setPath(http::Method::GET, "/multi-status", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.status(http::StatusCodeNotFound);  // Should override
    writer.status(http::StatusCodeInternalServerError);
    writer.reason("Custom Reason");  // Should override again
    writer.end();
  });

  const std::string response = test::simpleGet(port, "/multi-status");
  EXPECT_TRUE(response.contains("500"));
  EXPECT_TRUE(response.contains("Custom Reason"));
}

// Test contentLength called when writer is in Failed state - should log "writer-failed" reason
// Note: The Failed state is difficult to trigger in integration tests as it requires connection failure.
// This test attempts to trigger failure by rapidly closing/aborting the connection while the handler
// tries to write large amounts of data that might exceed socket buffers.
// Ignore SIGPIPE to prevent process termination on broken pipe

// Test: ensureHeadersSent() enqueue failure (line 141)
// Trigger failure when first sending headers by closing connection immediately
TEST(HttpResponseWriterFailures, EnsureHeadersSentFailure) {
#ifdef AERONET_POSIX
  ::signal(SIGPIPE, SIG_IGN);  // NOLINT(misc-include-cleaner)
#endif
  ts.router().setPath(http::Method::GET, "/ensure-headers-sent-fail",
                      [](const HttpRequestView&, HttpResponseWriter& writer) {
                        writer.status(http::StatusCodeOK);
                        // writeBody triggers ensureHeadersSent internally
                        writer.writeBody("data");
                        writer.end();
                      });

  test::ClientConnection sock(port);
  auto fd = sock.fd();
  std::string req = "GET /ensure-headers-sent-fail HTTP/1.1\r\nHost: test\r\n\r\n";
  test::sendAll(fd, req);
  // Close immediately to cause header enqueue to fail
  aeronet::CloseNativeHandle(fd);
}

// Test: emitLastChunk() enqueue failure (line 190)
// Chunked response with trailer, close connection to fail last chunk
TEST(HttpResponseWriterFailures, EmitLastChunkFailure) {
#ifdef AERONET_POSIX
  ::signal(SIGPIPE, SIG_IGN);  // NOLINT(misc-include-cleaner)
#endif
  ts.router().setPath(http::Method::GET, "/last-chunk-fail", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("chunk1");
    writer.trailerAddLine("X-Trailer", "value");
    EXPECT_THROW(writer.trailerAddLine("Invalid:header", "value"), std::invalid_argument);
    EXPECT_THROW(writer.trailerAddLine("X-Trailer", "value\r\n"), std::invalid_argument);
    // end() calls emitLastChunk
    writer.end();
  });

  test::ClientConnection sock(port);
  auto fd = sock.fd();
  std::string req = "GET /last-chunk-fail HTTP/1.1\r\nHost: test\r\n\r\n";
  test::sendAll(fd, req);
  // Read some data first
  char buf[1024];
  ::recv(fd, buf, sizeof(buf), 0);
  // Close before last chunk
  aeronet::CloseNativeHandle(fd);
}

// Test: writeBody() fixed-length enqueue failure (line 233)
// Non-chunked response with HEAD request (fixed length path), close to fail body write
TEST(HttpResponseWriterFailures, WriteBodyFixedLengthFailure) {
#ifdef AERONET_POSIX
  ::signal(SIGPIPE, SIG_IGN);  // NOLINT(misc-include-cleaner)
#endif
  ts.router().setPath(http::Method::HEAD, "/fixed-body-fail", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentLength(1000);
    // HEAD request uses fixed-length path (not chunked)
    // Try to write body data (will be suppressed for HEAD)
    for (int i = 0; i < 100; ++i) {
      writer.writeBody("data");
    }
    writer.end();
  });

  test::ClientConnection sock(port);
  auto fd = sock.fd();
  std::string req = "HEAD /fixed-body-fail HTTP/1.1\r\nHost: test\r\n\r\n";
  test::sendAll(fd, req);
  aeronet::CloseNativeHandle(fd);
}

// Enable compression, close connection to fail final compressed output
TEST(HttpResponseWriterFailures, EndCompressionFailure) {
#ifdef AERONET_POSIX
  ::signal(SIGPIPE, SIG_IGN);  // NOLINT(misc-include-cleaner)
#endif
      // Enable compression in server config
  HttpServerConfig cfg;
  cfg.compression.minBytes = 16;
  test::TestServer ts2(cfg);

  ts2.router().setPath(http::Method::GET, "/compress-end-fail", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    // Write enough to trigger compression
    writer.writeBody("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");  // 30 bytes
    // end() calls encoder flush which may produce output
    writer.end();
  });

  test::ClientConnection sock(ts2.port());
  auto fd = sock.fd();
  std::string req = "GET /compress-end-fail HTTP/1.1\r\nHost: test\r\nAccept-Encoding: gzip\r\n\r\n";
  test::sendAll(fd, req);
  // Read headers
  char buf[512];
  ::recv(fd, buf, sizeof(buf), 0);
  // Close before final encoder output
  aeronet::CloseNativeHandle(fd);
}

// Small write below compression threshold, close to fail buffered flush
TEST(HttpResponseWriterFailures, EndIdentityBufferedFailure) {
#ifdef AERONET_POSIX
  ::signal(SIGPIPE, SIG_IGN);  // NOLINT(misc-include-cleaner)
#endif
      // Enable compression but write below threshold
  HttpServerConfig cfg;
  cfg.compression.minBytes = 100;  // High threshold
  test::TestServer ts3(cfg);

  ts3.router().setPath(http::Method::GET, "/identity-buffered-fail",
                       [](const HttpRequestView&, HttpResponseWriter& writer) {
                         writer.status(http::StatusCodeOK);
                         // Write small amount (below compression threshold)
                         writer.writeBody("small");  // Only 5 bytes, below 100
                         // end() flushes buffered identity data
                         writer.end();
                       });

  test::ClientConnection sock(ts3.port());
  auto fd = sock.fd();
  std::string req = "GET /identity-buffered-fail HTTP/1.1\r\nHost: test\r\n\r\n";
  test::sendAll(fd, req);
  // Read headers
  char buf[512];
  ::recv(fd, buf, sizeof(buf), 0);
  // Close before buffered flush
  aeronet::CloseNativeHandle(fd);
}

// This was covered by the original ContentLengthWhenFailed test
TEST(HttpResponseWriterFailures, EmitChunkFailure) {
#ifdef AERONET_POSIX
  ::signal(SIGPIPE, SIG_IGN);  // NOLINT(misc-include-cleaner)
#endif
  std::string largeData(10000, 'x');
  ts.router().setPath(http::Method::GET, "/emit-chunk-fail",
                      [largeData](const HttpRequestView&, HttpResponseWriter& writer) {
                        writer.status(http::StatusCodeOK);
                        for (int i = 0; i < 100; ++i) {
                          if (!writer.writeBody(largeData)) {
                            writer.contentLength(999);
                            writer.trailerAddLine("X-Fail", "yes");
                            writer.end();
                            return;
                          }
                        }
                        writer.end();
                      });

  test::ClientConnection sock(port);
  auto fd = sock.fd();
  std::string req = "GET /emit-chunk-fail HTTP/1.1\r\nHost: test\r\n\r\n";
  test::sendAll(fd, req);
  aeronet::CloseNativeHandle(fd);
}

// Test contentLength called when writer is in Ended state
TEST(HttpResponseWriterFailures, ContentLengthWhenEnded) {
  ts.router().setPath(http::Method::GET, "/content-length-after-ended",
                      [](const HttpRequestView&, HttpResponseWriter& writer) {
                        writer.status(http::StatusCodeOK);
                        writer.writeBody("body-data");
                        writer.end();
                        writer.contentLength(50);
                      });

  const std::string response = test::simpleGet(port, "/content-length-after-ended");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(response.contains("body-data"));
}

// Test contentLength called when writer is in HeadersSent state
TEST(HttpResponseWriterFailures, ContentLengthAfterHeadersSent) {
  ts.router().setPath(http::Method::GET, "/content-length-headers-sent",
                      [](const HttpRequestView&, HttpResponseWriter& writer) {
                        writer.status(http::StatusCodeOK);
                        writer.writeBody("first-chunk");
                        writer.contentLength(200);
                        writer.writeBody("second-chunk");
                        writer.end();
                      });

  const std::string response = test::simpleGet(port, "/content-length-headers-sent");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(response.contains("first-chunk"));
  EXPECT_TRUE(response.contains("second-chunk"));
}

TEST(HttpStreamingMakeResponse, PrefillsGlobalHeadersHttp11) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.addGlobalHeader(http::Header{"X-Global", "gvalue"});
    cfg.addGlobalHeader(http::Header{"X-Another", "anothervalue"});
  });

  ts.router().setPath(http::Method::GET, "/stream-make-response",
                      [](const HttpRequestView& req, HttpResponseWriter& writer) {
                        auto base = req.makeResponse(http::StatusCodeAccepted, "ignored", http::ContentTypeTextPlain);

                        writer.status(base.status());
                        if (auto val = base.headerValue("X-Global")) {
                          writer.headerAddLine("X-Global", *val);
                        }
                        if (auto val = base.headerValue("X-Another")) {
                          writer.headerAddLine("X-Another", *val);
                        }
                        writer.headerAddLine("X-Stream", "yes");

                        writer.writeBody("stream-body");
                        writer.end();
                      });

  test::ClientConnection client(port);
  const auto fd = client.fd();
  std::string req = "GET /stream-make-response HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  const std::string resp = test::recvUntilClosed(fd);

  EXPECT_TRUE(resp.starts_with("HTTP/1.1 202"));
  EXPECT_TRUE(resp.contains("X-Global: gvalue"));
  EXPECT_TRUE(resp.contains("X-Another: anothervalue"));
  EXPECT_TRUE(resp.contains("X-Stream: yes"));
  EXPECT_TRUE(resp.contains("stream-body"));
}

// Test chunked request body with Expect: 100-continue header.
// Verifies the server correctly sends 100 Continue before reading the chunked body.
TEST(HttpStreaming, ChunkedRequestWithExpect100Continue) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.enableKeepAlive = false; });

  ts.router().setPath(http::Method::POST, "/chunked-expect",
                      [](const HttpRequestView& req) { return req.makeResponse(req.body()); });

  test::ClientConnection cnx(port);
  auto fd = cnx.fd();

  // Send headers first with Expect: 100-continue
  std::string headers =
      "POST /chunked-expect HTTP/1.1\r\n"
      "Host: test\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Expect: 100-continue\r\n"
      "Connection: close\r\n\r\n";
  test::sendAll(fd, headers);

  // Wait for 100 Continue interim response
  static constexpr std::string_view kExpected100Continue = "HTTP/1.1 100 Continue\r\n\r\n";
  std::string interim = test::recvWithTimeout(fd, std::chrono::seconds(1), kExpected100Continue.size());
  EXPECT_EQ(interim, kExpected100Continue) << "Expected 100 Continue response, got: " << interim;

  // Now send the chunked body
  std::string chunkedBody =
      "5\r\nhello\r\n"
      "6\r\n world\r\n"
      "0\r\n\r\n";
  test::sendAll(fd, chunkedBody);

  // Collect final response
  std::string full = interim + test::recvUntilClosed(fd);
  EXPECT_TRUE(full.contains("HTTP/1.1 200")) << "Expected 200 OK response, got: " << full;
  EXPECT_TRUE(full.contains("hello world")) << "Expected echoed body, got: " << full;
}

#if defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZSTD)
// Test chunked request with malformed Content-Encoding header.
// When decompression is enabled, a malformed Content-Encoding (e.g., empty token)
// should be rejected with 400 Bad Request during chunked body decoding.
TEST(HttpStreaming, ChunkedRequestWithMalformedContentEncodingRejects400) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.enableKeepAlive = false;
    cfg.decompression = {};  // Enable decompression with defaults
  });

  ts.router().setPath(http::Method::POST, "/chunked-bad-encoding",
                      [](const HttpRequestView& req) { return HttpResponse(req.body()); });

  test::ClientConnection cnx(port);
  auto fd = cnx.fd();

  // Send chunked request with malformed Content-Encoding (double comma = empty token)
  std::string req =
      "POST /chunked-bad-encoding HTTP/1.1\r\n"
      "Host: test\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Content-Encoding: identity,,identity\r\n"
      "Connection: close\r\n\r\n"
      "3\r\nabc\r\n"
      "0\r\n\r\n";
  test::sendAll(fd, req);

  std::string resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 400"))
      << "Expected 400 Bad Request for malformed Content-Encoding, got: " << resp;
}

// Test chunked request with empty Content-Encoding header value.
TEST(HttpStreaming, ChunkedRequestWithEmptyContentEncodingRejects400) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.enableKeepAlive = false;
    cfg.decompression = {};  // Enable decompression with defaults
  });

  ts.router().setPath(http::Method::POST, "/chunked-empty-encoding",
                      [](const HttpRequestView& req) { return HttpResponse(req.body()); });

  test::ClientConnection cnx(port);
  auto fd = cnx.fd();

  // Send chunked request with empty Content-Encoding header value
  std::string req =
      "POST /chunked-empty-encoding HTTP/1.1\r\n"
      "Host: test\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Content-Encoding: \r\n"
      "Connection: close\r\n\r\n"
      "3\r\nxyz\r\n"
      "0\r\n\r\n";
  test::sendAll(fd, req);

  std::string resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 400")) << "Expected 400 Bad Request for empty Content-Encoding, got: " << resp;
}

TEST(HttpStreaming, ChunkedRequestMalformedCRLF) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.enableKeepAlive = false; });

  ts.router().setPath(http::Method::POST, "/chunked-malformed-crlf",
                      [](const HttpRequestView& req) { return HttpResponse(req.body()); });

  test::ClientConnection cnx(port);
  auto fd = cnx.fd();

  // Send chunked request with malformed CRLF after chunk data
  std::string req =
      "POST /chunked-malformed-crlf HTTP/1.1\r\n"
      "Host: test\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n\r\n"
      "3\r\nabc\n\n"  // Malformed CRLF here
      "0\r\n\r\n";
  test::sendAll(fd, req);

  std::string resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 400")) << "Expected 400 Bad Request for malformed CRLF, got: " << resp;
}

TEST(HttpStreaming, ChunkedRequestPayloadTooLargeNoDecompression) {
  ts.router() = Router();
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.enableKeepAlive = false;
    cfg.maxBodyBytes = 5;              // Very small max body
    cfg.decompression.enable = false;  // Disable decompression to hit the specific branch
  });

  ts.router().setPath(http::Method::POST, "/chunked-too-large",
                      [](const HttpRequestView& req) { return HttpResponse(req.body()); });

  test::ClientConnection cnx(port);
  auto fd = cnx.fd();

  // Send chunked request with body larger than maxBodyBytes
  std::string req =
      "POST /chunked-too-large HTTP/1.1\r\n"
      "Host: test\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n\r\n"
      "3\r\n123\r\n"
      "3\r\n456\r\n"
      "0\r\n\r\n";
  test::sendAll(fd, req);

  std::string resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 413")) << "Expected 413 Payload Too Large, got: " << resp;
}

#endif

TEST(HttpRangeStatic, ServeCompleteFile) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  EXPECT_EQ(parsed.body, "abcdefghij");
  EXPECT_EQ(getHeader(parsed, http::AcceptRanges), "bytes");
  EXPECT_FALSE(getHeader(parsed, http::ETag).empty());
  EXPECT_FALSE(getHeader(parsed, http::LastModified).empty());
}

TEST(HttpRangeStatic, SingleRangePartialContent) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-3");

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);
  EXPECT_EQ(parsed.body, "abcd");
  EXPECT_EQ(getHeader(parsed, http::ContentRange), "bytes 0-3/10");
}

TEST(HttpRangeStatic, UnsatisfiableRange) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=100-200");

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeRangeNotSatisfiable);
  EXPECT_EQ(getHeader(parsed, http::ContentRange), "bytes */10");
}

TEST(HttpRangeStatic, IfNoneMatchReturns304) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions initial;
  initial.method = "GET";
  initial.target = "/" + fileName;
  const auto firstRaw = test::requestOrThrow(ts.port(), initial);
  const auto firstParsed = test::parseResponseOrThrow(firstRaw);
  const auto etag = getHeader(firstParsed, http::ETag);
  ASSERT_FALSE(etag.empty());

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("If-None-Match", etag);

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeNotModified);
  EXPECT_TRUE(parsed.body.empty());
}

TEST(HttpRangeStatic, IfRangeMismatchFallsBackToFullBody) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-3");
  opt.headers.emplace_back("If-Range", "\"mismatch\"");

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  EXPECT_EQ(parsed.body, "abcdefghij");
}

TEST(HttpRangeInvalid, BadRangeSyntax) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "0123456789");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;

  // Non-numeric start
  opt.headers.clear();
  opt.headers.emplace_back("Range", "bytes=abc-4");
  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeRangeNotSatisfiable);

  // Multiple adjacent ranges → now coalesced to single valid range (bytes 0-3)
  opt.headers.clear();
  opt.headers.emplace_back("Range", "bytes=0-1,2-3");
  raw = test::requestOrThrow(ts.port(), opt);
  parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);
  EXPECT_EQ(getHeader(parsed, http::ContentRange), "bytes 0-3/10");
  EXPECT_EQ(parsed.body, "0123");

  // Suffix zero is invalid (bytes=-0)
  opt.headers.clear();
  opt.headers.emplace_back("Range", "bytes=-0");
  raw = test::requestOrThrow(ts.port(), opt);
  parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeRangeNotSatisfiable);
}

TEST(HttpRangeInvalid, ConditionalInvalidDates) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "hello world");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;

  // If-Modified-Since with an invalid date should be ignored -> full body returned
  opt.headers.clear();
  opt.headers.emplace_back("If-Modified-Since", "Not a date");
  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  EXPECT_EQ(parsed.body, "hello world");

  // If-Unmodified-Since invalid date should be ignored (no 412)
  opt.headers.clear();
  opt.headers.emplace_back("If-Unmodified-Since", "garbage-date");
  raw = test::requestOrThrow(ts.port(), opt);
  parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
}

TEST(HttpRangeInvalid, IfMatchPreconditionFailed) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "HELLO");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  // First fetch to get ETag
  test::RequestOptions initial;
  initial.method = "GET";
  initial.target = "/" + fileName;
  const auto firstRaw = test::requestOrThrow(ts.port(), initial);
  const auto firstParsed = test::parseResponseOrThrow(firstRaw);
  const auto headers = firstParsed.headers;
  const auto etag = headers.find(http::ETag);
  ASSERT_NE(etag, headers.end());

  // If-Match with a non-matching tag -> 412 Precondition Failed
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("If-Match", "\"no-match\"");

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePreconditionFailed);
}

TEST(HttpLargeFile, ServeLargeFile) {
  const std::uint64_t size = 16ULL * 1024ULL * 1024ULL;
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp([&]() {
    std::string data;
    data.assign(static_cast<std::size_t>(size), '\0');
    for (std::uint64_t i = 0; i < size; ++i) {
      data[static_cast<std::size_t>(i)] = static_cast<char>('a' + (i % 26));
    }
    return test::ScopedTempFile(tmpDir, data);
  }());
  const auto fileName = tmp.filename();
  const std::string_view data = tmp.content();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  // Use a custom connection to manually control receive behavior for large files
  test::ClientConnection cnx(ts.port());
  NativeHandle fd = cnx.fd();

  std::string req = "GET /" + fileName + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);

  // Use recvWithTimeout which waits for complete Content-Length
  const auto raw = test::recvWithTimeout(fd, std::chrono::seconds(10));

  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  EXPECT_EQ(parsed.body.size(), size);
  const auto headers = parsed.headers;
  const auto it = headers.find(http::ContentLength);
  ASSERT_NE(it, headers.end());
  EXPECT_EQ(StringToIntegral<std::uint64_t>(it->second), size);
  EXPECT_TRUE(parsed.body == data);
}

// ---------------------------------------------------------------------------
// Multipart / multi-range integration tests (RFC 7233 multipart/byteranges)
// ---------------------------------------------------------------------------

namespace {

struct MultipartPart {
  std::string contentType;
  std::string contentRange;
  std::string body;
};

vector<MultipartPart> ParseMultipartByterangesResponse(const std::string& ctHeader, const std::string& body) {
  auto bpos = ctHeader.find("boundary=");
  if (bpos == std::string::npos) {
    return {};
  }
  std::string boundary = ctHeader.substr(bpos + 9);

  std::string delim = "\r\n--" + boundary;

  vector<MultipartPart> parts;
  std::string_view remaining = body;

  auto firstPos = remaining.find(delim);
  if (firstPos == std::string_view::npos) {
    return {};
  }
  remaining.remove_prefix(firstPos + delim.size());

  while (true) {
    if (remaining.starts_with("--")) {
      break;
    }
    if (!remaining.starts_with(http::CRLF)) {
      break;
    }
    remaining.remove_prefix(http::CRLF.size());

    auto headerEnd = remaining.find(http::DoubleCRLF);
    if (headerEnd == std::string_view::npos) {
      break;
    }
    auto headerBlock = remaining.substr(0, headerEnd);
    remaining.remove_prefix(headerEnd + http::DoubleCRLF.size());

    MultipartPart part;
    while (!headerBlock.empty()) {
      auto lineEnd = headerBlock.find(http::CRLF);
      auto line = (lineEnd == std::string_view::npos) ? headerBlock : headerBlock.substr(0, lineEnd);
      auto colon = line.find(':');
      if (colon != std::string_view::npos) {
        auto key = line.substr(0, colon);
        auto val = line.substr(colon + 1);
        while (!val.empty() && val.front() == ' ') {
          val.remove_prefix(1);
        }
        if (key == "Content-Type") {
          part.contentType = std::string(val);
        } else if (key == "Content-Range") {
          part.contentRange = std::string(val);
        }
      }
      if (lineEnd == std::string_view::npos) {
        break;
      }
      headerBlock.remove_prefix(lineEnd + http::CRLF.size());
    }

    auto nextDelim = remaining.find(delim);
    if (nextDelim == std::string_view::npos) {
      break;
    }
    part.body = std::string(remaining.substr(0, nextDelim));
    remaining.remove_prefix(nextDelim + delim.size());
    parts.push_back(std::move(part));
  }
  return parts;
}

}  // namespace

TEST(HttpRangeMulti, MultiRangePartialContent) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-3, 6-9");

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);

  const auto ct = getHeader(parsed, http::ContentType);
  EXPECT_TRUE(ct.starts_with("multipart/byteranges; boundary=")) << "Got: " << ct;

  auto parts = ParseMultipartByterangesResponse(ct, parsed.body);
  ASSERT_EQ(parts.size(), 2U);
  EXPECT_EQ(parts[0].body, "abcd");
  EXPECT_EQ(parts[1].body, "ghij");
  EXPECT_EQ(parts[0].contentRange, "bytes 0-3/10");
  EXPECT_EQ(parts[1].contentRange, "bytes 6-9/10");
}

TEST(HttpRangeMulti, MultiRangeBodyPartsMatchFileContent) {
  test::ScopedTempDir tmpDir;
  const std::string content = "The quick brown fox jumps over the lazy dog";
  test::ScopedTempFile tmp(tmpDir, content);
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-2, 10-14, 35-42");

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);

  auto parts = ParseMultipartByterangesResponse(getHeader(parsed, http::ContentType), parsed.body);
  ASSERT_EQ(parts.size(), 3U);
  EXPECT_EQ(parts[0].body, "The");
  EXPECT_EQ(parts[1].body, "brown");
  EXPECT_EQ(parts[2].body, "lazy dog");
}

TEST(HttpRangeMulti, MultiRangeCoalescedResponse) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-5, 3-9");  // overlapping → coalesced to 0-9

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);
  // Coalesced to single range → simple Content-Range
  EXPECT_EQ(getHeader(parsed, http::ContentRange), "bytes 0-9/10");
  EXPECT_EQ(parsed.body, "abcdefghij");
}

TEST(HttpRangeMulti, MultiRangeSingleSatisfiable) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-3, 100-200");  // second unsatisfiable

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);
  EXPECT_EQ(getHeader(parsed, http::ContentRange), "bytes 0-3/10");
  EXPECT_EQ(parsed.body, "abcd");
}

TEST(HttpRangeMulti, MultiRangeIfRangeInteraction) {
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, "abcdefghij");
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  // First get the ETag
  test::RequestOptions initial;
  initial.method = "GET";
  initial.target = "/" + fileName;
  const auto firstRaw = test::requestOrThrow(ts.port(), initial);
  const auto firstParsed = test::parseResponseOrThrow(firstRaw);
  const auto etag = getHeader(firstParsed, http::ETag);
  ASSERT_FALSE(etag.empty());

  // Multi-range with matching If-Range → 206 multipart
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-3, 6-9");
  opt.headers.emplace_back("If-Range", etag);

  auto raw = test::requestOrThrow(ts.port(), opt);
  auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);
  EXPECT_TRUE(getHeader(parsed, http::ContentType).starts_with("multipart/byteranges"));

  // Multi-range with mismatched If-Range → 200 full body
  opt.headers.clear();
  opt.headers.emplace_back("Range", "bytes=0-3, 6-9");
  opt.headers.emplace_back("If-Range", "\"mismatch\"");

  raw = test::requestOrThrow(ts.port(), opt);
  parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodeOK);
  EXPECT_EQ(parsed.body, "abcdefghij");
}

TEST(HttpRangeMulti, MultiRangeLargeFile) {
  // File larger than inlineFileThresholdBytes
  test::ScopedTempDir tmpDir;
  std::string content(256UL * 1024, '\0');  // 256 KiB
  for (std::size_t i = 0; i < content.size(); ++i) {
    content[i] = static_cast<char>('a' + (i % 26));
  }
  test::ScopedTempFile tmp(tmpDir, content);
  const std::string fileName = tmp.filename();

  ts.router().setDefault(StaticFileHandler(tmp.dirPath()));

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/" + fileName;
  opt.headers.emplace_back("Range", "bytes=0-9, 1000-1009, 100000-100009");

  const auto raw = test::requestOrThrow(ts.port(), opt);
  const auto parsed = test::parseResponseOrThrow(raw);
  EXPECT_EQ(parsed.statusCode, http::StatusCodePartialContent);

  auto parts = ParseMultipartByterangesResponse(getHeader(parsed, http::ContentType), parsed.body);
  ASSERT_EQ(parts.size(), 3U);
  EXPECT_EQ(parts[0].body, content.substr(0, 10));
  EXPECT_EQ(parts[1].body, content.substr(1000, 10));
  EXPECT_EQ(parts[2].body, content.substr(100000, 10));
}

TEST(HttpQueryParsing, NoQuery) {
  ts.router().setPath(http::Method::GET, "/plain", [](const HttpRequestView& req) {
    EXPECT_EQ(req.path(), "/plain");
    EXPECT_EQ(req.queryParams().begin(), req.queryParams().end());
    HttpResponse resp;
    resp.status(200).reason("OK").body("NOQ");
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/plain");
  EXPECT_TRUE(resp.contains("NOQ"));
}

TEST(HttpQueryParsing, SimpleQuery) {
  ts.router().setPath(http::Method::GET, "/p", [](const HttpRequestView& req) {
    EXPECT_EQ(req.path(), "/p");

    std::string body;
    for (const auto& [key, val] : req.queryParams()) {
      if (!body.empty()) {
        body.push_back('&');
      }
      body.append(key);
      body.push_back('=');
      body.append(val);
    }

    return HttpResponse(body);
  });
  auto resp = test::simpleGet(ts.port(), "/p?a=1&b=2");
  EXPECT_TRUE(resp.contains("a=1&b=2"));
}

TEST(HttpQueryParsing, PercentDecodedQuery) {
  ts.router().setPath(http::Method::GET, "/d", [](const HttpRequestView& req) -> HttpResponse {
    // Query is now fully percent-decoded by parser.
    EXPECT_EQ(req.path(), "/d");
    auto range = req.queryParamsRange();
    auto it = range.begin();
    EXPECT_NE(it, range.end());
    EXPECT_EQ((*it).key, "x");
    EXPECT_EQ((*it).value, "one two");  // %20 decoded
    ++it;
    EXPECT_NE(it, range.end());
    EXPECT_EQ((*it).key, "y");
    EXPECT_EQ((*it).value, "/path");  // %2F decoded
    ++it;
    EXPECT_EQ(it, range.end());
    HttpResponse resp;
    // Echo decoded query back in body for client-side verification
    std::string body;
    for (const auto& [key, val] : req.queryParamsRange()) {
      if (!body.empty()) {
        body.push_back('&');
      }
      body.append(key);
      body.push_back('=');
      body.append(val);
    }
    resp.status(200).reason("OK").body(body);
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/d?x=one%20two&y=%2Fpath");
  // Body should contain decoded query string now.
  EXPECT_TRUE(resp.contains("x=one two&y=/path"));
}

TEST(HttpQueryParsing, EmptyQueryAndTrailingQMark) {
  ts.router().setPath(http::Method::GET, "/t", [](const HttpRequestView& req) {
    EXPECT_EQ(req.path(), "/t");
    // "?" with nothing after -> empty query view
    EXPECT_EQ(req.queryParams().begin(), req.queryParams().end());
    HttpResponse resp;
    resp.status(200).reason("OK").body("EMPTY");
    return resp;
  });
  auto resp = test::simpleGet(ts.port(), "/t?");
  EXPECT_TRUE(resp.contains("EMPTY"));
}

TEST(HttpQueryParsingEdge, IncompleteEscapeAtEndShouldBeAccepted) {
  ts.router().setPath(http::Method::GET, "/e", [](const HttpRequestView& req) -> HttpResponse {
    EXPECT_EQ(req.path(), "/e");
    // "%" at end remains literal
    // Malformed escape -> fallback leaves query raw
    auto it = req.queryParamsRange().begin();
    EXPECT_NE(it, req.queryParamsRange().end());
    EXPECT_EQ((*it).key, "x");
    EXPECT_EQ((*it).value, "%");
    HttpResponse resp(200);
    resp.reason("OK");
    resp.body("EDGE1");
    return resp;
  });
  std::string out = test::simpleGet(ts.port(), "/e?x=%");
  EXPECT_TRUE(out.starts_with("HTTP/1.1 200 OK"));
  EXPECT_TRUE(out.ends_with("\r\n\r\nEDGE1"));
}

TEST(HttpQueryParsingEdge, IncompleteEscapeOneHexShouldBeAccepted) {
  ts.router().setPath(http::Method::GET, "/e2", [](const HttpRequestView& req) -> HttpResponse {
    auto it = req.queryParamsRange().begin();
    EXPECT_NE(it, req.queryParamsRange().end());
    EXPECT_EQ((*it).key, "a");
    // Invalid -> left as literal
    EXPECT_EQ((*it).value, "%A");
    return HttpResponse("EDGE2");
  });
  std::string resp = test::simpleGet(ts.port(), "/e2?a=%A");

  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(resp.ends_with("\r\n\r\nEDGE2"));
}

TEST(HttpQueryParsingEdge, MultiplePairsAndEmptyValue) {
  ts.router().setPath(http::Method::GET, "/m", [](const HttpRequestView& req) -> HttpResponse {
    auto range = req.queryParamsRange();
    // Check that HttpRequestView::queryParamsRange works with std::ranges
    for (const auto& [index, keyValue] : range | std::views::enumerate) {
      if (index == 0) {
        EXPECT_EQ(keyValue.key, "k");
        EXPECT_EQ(keyValue.value, "1");
      } else if (index == 1) {
        EXPECT_EQ(keyValue.key, "empty");
        EXPECT_EQ(keyValue.value, "");
      } else if (index == 2) {
        EXPECT_EQ(keyValue.key, "novalue");
        EXPECT_EQ(keyValue.value, "");
      } else {
        throw std::logic_error("Too many query params");
      }
    }
    auto it = range.begin();
    EXPECT_NE(it, range.end());
    EXPECT_EQ((*it).key, "k");
    EXPECT_EQ((*it).value, "1");
    ++it;
    EXPECT_NE(it, range.end());
    EXPECT_EQ((*it).key, "empty");
    EXPECT_EQ((*it).value, "");
    ++it;
    EXPECT_NE(it, range.end());
    EXPECT_EQ((*it).key, "novalue");
    EXPECT_EQ((*it).value, "");
    ++it;
    EXPECT_EQ(it, range.end());
    return HttpResponse("EDGE3");
  });
  std::string resp = test::simpleGet(ts.port(), "/m?k=1&empty=&novalue");
  EXPECT_TRUE(resp.ends_with("\r\n\r\nEDGE3"));
}

TEST(HttpQueryParsingEdge, PercentDecodingKeyAndValue) {
  ts.router().setPath(http::Method::GET, "/pd", [](const HttpRequestView& req) -> HttpResponse {
    // encoded: %66 -> 'f'
    // Fully decodable -> parser decodes now
    auto it = req.queryParamsRange().begin();
    EXPECT_NE(it, req.queryParamsRange().end());
    EXPECT_EQ((*it).key, "fo");
    EXPECT_EQ((*it).value, "bar baz");
    HttpResponse resp;
    resp.status(200).reason("OK").body("EDGE4");
    return resp;
  });
  std::string resp = test::simpleGet(ts.port(), "/pd?%66o=bar%20baz");

  EXPECT_TRUE(resp.contains("EDGE4"));
}

TEST(HttpQueryStructuredBindings, IterateKeyValues) {
  ts.router().setPath(http::Method::GET, "/sb", [](const HttpRequestView& req) -> HttpResponse {
    EXPECT_EQ(req.path(), "/sb");
    int count = 0;
    bool sawA = false;
    bool sawB = false;
    bool sawEmpty = false;
    bool sawNoValue = false;
    for (const auto& [key, value] : req.queryParams()) {
      ++count;
      if (key == "a") {
        EXPECT_EQ(value, "1");
        sawA = true;
      } else if (key == "b") {
        EXPECT_EQ(value, "two words");
        sawB = true;
      } else if (key == "empty") {
        EXPECT_TRUE(value.empty());
        sawEmpty = true;
      } else if (key == "novalue") {
        EXPECT_TRUE(value.empty());
        sawNoValue = true;
      }
    }
    EXPECT_EQ(count, 4);
    EXPECT_TRUE(sawA);
    EXPECT_TRUE(sawB);
    EXPECT_TRUE(sawEmpty);
    EXPECT_TRUE(sawNoValue);
    EXPECT_EQ(req.queryParamValue("a").value_or(""), "1");
    EXPECT_FALSE(req.queryParamValue("c"));
    EXPECT_EQ(req.queryParamValueOrEmpty("b"), "two words");
    EXPECT_EQ(req.queryParamValueOrEmpty("c"), "");
    EXPECT_TRUE(req.hasQueryParam("empty"));
    EXPECT_TRUE(req.hasQueryParam("novalue"));
    EXPECT_FALSE(req.hasQueryParam("missing"));
    return HttpResponse(http::StatusCodeOK);
  });
  // Build raw HTTP request using helpers
  test::ClientConnection client(ts.port());
  std::string req = "GET /sb?a=1&b=two%20words&empty=&novalue HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
  test::sendAll(client.fd(), req);
  auto resp = test::recvUntilClosed(client.fd());
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
}

namespace {

class ThrowingRateLimitStore final : public IRateLimitStore {
 public:
  RateLimitDecision consume([[maybe_unused]] std::string_view key,
                            [[maybe_unused]] std::chrono::steady_clock::time_point now,
                            [[maybe_unused]] const RateLimitConfig& config) override {
    throw std::runtime_error("store failure");
  }
};

auto okHandler() {
  return [](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK, "ok"); };
}

}  // namespace

TEST(HttpRateLimit, GlobalPeerAddressLimiterReturns429AndRetryAfter) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/rl", okHandler());

  RateLimitRequestMiddlewareBuilder opts;
  opts.config.requestsPerSecond = 1;
  opts.config.burst = 1;
  opts.keyStrategy = RateLimitClientKeyStrategy::PeerAddress;
  router.addRequestMiddleware(std::move(opts).build());

  const auto r1 = aeronet::test::simpleGet(ts.port(), "/rl");
  EXPECT_TRUE(r1.starts_with("HTTP/1.1 200")) << r1;

  const auto r2 = aeronet::test::simpleGet(ts.port(), "/rl");
  EXPECT_TRUE(r2.starts_with("HTTP/1.1 429")) << r2;
  EXPECT_TRUE(r2.contains("Retry-After:")) << r2;
}

TEST(HttpRateLimit, RouteSpecificLimiterOnlyAffectsItsRoute) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/strict", okHandler()).before([] {
    RateLimitRequestMiddlewareBuilder options;
    options.config.requestsPerSecond = 1;
    options.config.burst = 1;
    return std::move(options).build();
  }());
  router.setPath(http::Method::GET, "/open", okHandler());

  const auto strict1 = aeronet::test::simpleGet(ts.port(), "/strict");
  const auto strict2 = aeronet::test::simpleGet(ts.port(), "/strict");
  const auto open1 = aeronet::test::simpleGet(ts.port(), "/open");
  const auto open2 = aeronet::test::simpleGet(ts.port(), "/open");

  EXPECT_TRUE(strict1.starts_with("HTTP/1.1 200")) << strict1;
  EXPECT_TRUE(strict2.starts_with("HTTP/1.1 429")) << strict2;
  EXPECT_TRUE(open1.starts_with("HTTP/1.1 200")) << open1;
  EXPECT_TRUE(open2.starts_with("HTTP/1.1 200")) << open2;
}

TEST(HttpRateLimit, ForwardedForStrategyUsesHeaderKey) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/xff", okHandler());

  router.addRequestMiddleware(RateLimitRequestMiddlewareBuilder{
      .config = RateLimitConfig{.requestsPerSecond = 1, .burst = 1},
      .store = {},
      .keyStrategy = RateLimitClientKeyStrategy::XForwardedForFirst,
      .customKeyExtractor = {},
  }
                                  .build());

  aeronet::test::RequestOptions req;
  req.method = "GET";
  req.target = "/xff";
  req.headers.emplace_back("X-Forwarded-For", "203.0.113.7");

  const auto r1 = aeronet::test::requestOrThrow(ts.port(), req);
  const auto r2 = aeronet::test::requestOrThrow(ts.port(), req);

  EXPECT_TRUE(r1.starts_with("HTTP/1.1 200")) << r1;
  EXPECT_TRUE(r2.starts_with("HTTP/1.1 429")) << r2;
}

TEST(HttpRateLimit, ForwardedForStrategyBypassesWhenHeaderMissing) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/xff-missing", okHandler());

  RateLimitRequestMiddlewareBuilder options;
  options.config.requestsPerSecond = 1;
  options.config.burst = 1;
  options.keyStrategy = RateLimitClientKeyStrategy::XForwardedForFirst;
  router.addRequestMiddleware(std::move(options).build());

  const auto r1 = aeronet::test::simpleGet(ts.port(), "/xff-missing");
  const auto r2 = aeronet::test::simpleGet(ts.port(), "/xff-missing");

  EXPECT_TRUE(r1.starts_with("HTTP/1.1 200")) << r1;
  EXPECT_TRUE(r2.starts_with("HTTP/1.1 200")) << r2;
}

TEST(HttpRateLimit, HeaderValueStrategyUsesTrimmedHeaderAndConstBuild) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/header-key", okHandler());

  RateLimitRequestMiddlewareBuilder options;
  options.config.requestsPerSecond = 1;
  options.config.burst = 1;
  options.keyStrategy = RateLimitClientKeyStrategy::HeaderValue;
  options.headerName = "x-api-key";
  router.addRequestMiddleware(options.build());

  aeronet::test::RequestOptions req;
  req.method = "GET";
  req.target = "/header-key";
  req.headers.emplace_back("X-Api-Key", "  client-a  ");

  const auto r1 = aeronet::test::requestOrThrow(ts.port(), req);
  const auto r2 = aeronet::test::requestOrThrow(ts.port(), req);

  EXPECT_TRUE(r1.starts_with("HTTP/1.1 200")) << r1;
  EXPECT_TRUE(r2.starts_with("HTTP/1.1 429")) << r2;
}

TEST(HttpRateLimit, HeaderValueStrategyRequiresHeaderName) {
  RateLimitRequestMiddlewareBuilder options;
  options.keyStrategy = RateLimitClientKeyStrategy::HeaderValue;
  options.headerName = {};

  EXPECT_THROW(static_cast<void>(options.build()), std::invalid_argument);
}

TEST(HttpRateLimit, CustomStrategyWithoutExtractorBypassesRequests) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/custom-none", okHandler());

  RateLimitRequestMiddlewareBuilder options;
  options.config.requestsPerSecond = 1;
  options.config.burst = 1;
  options.keyStrategy = RateLimitClientKeyStrategy::Custom;
  router.addRequestMiddleware(std::move(options).build());

  const auto r1 = aeronet::test::simpleGet(ts.port(), "/custom-none");
  const auto r2 = aeronet::test::simpleGet(ts.port(), "/custom-none");

  EXPECT_TRUE(r1.starts_with("HTTP/1.1 200")) << r1;
  EXPECT_TRUE(r2.starts_with("HTTP/1.1 200")) << r2;
}

TEST(HttpRateLimit, CustomStrategyUsesExtractorKey) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/custom-key", okHandler());

  RateLimitRequestMiddlewareBuilder options;
  options.config.requestsPerSecond = 1;
  options.config.burst = 1;
  options.keyStrategy = RateLimitClientKeyStrategy::Custom;
  options.customKeyExtractor = [](const HttpRequestView& request) { return request.headerValueOrEmpty("x-client-id"); };
  router.addRequestMiddleware(std::move(options).build());

  aeronet::test::RequestOptions req;
  req.method = "GET";
  req.target = "/custom-key";
  req.headers.emplace_back("X-Client-Id", "tenant-a");

  const auto r1 = aeronet::test::requestOrThrow(ts.port(), req);
  const auto r2 = aeronet::test::requestOrThrow(ts.port(), req);

  EXPECT_TRUE(r1.starts_with("HTTP/1.1 200")) << r1;
  EXPECT_TRUE(r2.starts_with("HTTP/1.1 429")) << r2;
}

TEST(HttpRateLimit, StoreExceptionsFailOpenWhenConfigured) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/throw-open", okHandler());

  RateLimitRequestMiddlewareBuilder options;
  options.config.failOpen = true;
  options.store = std::make_shared<ThrowingRateLimitStore>();
  router.addRequestMiddleware(std::move(options).build());

  const auto response = aeronet::test::simpleGet(ts.port(), "/throw-open");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 200")) << response;
}

TEST(HttpRateLimit, StoreExceptionsFailClosedWithRetryAfterAndBody) {
  auto router = ts.resetRouterAndGet();
  router.setPath(http::Method::GET, "/throw-closed", okHandler());

  RateLimitRequestMiddlewareBuilder options;
  options.config.failOpen = false;
  options.store = std::make_shared<ThrowingRateLimitStore>();
  options.rejectionBody = "slow down";
  router.addRequestMiddleware(std::move(options).build());

  const auto response = aeronet::test::simpleGet(ts.port(), "/throw-closed");
  EXPECT_TRUE(response.starts_with("HTTP/1.1 429")) << response;
  EXPECT_TRUE(response.contains("Retry-After: 1")) << response;
  EXPECT_TRUE(response.contains("slow down")) << response;
}

}  // namespace aeronet