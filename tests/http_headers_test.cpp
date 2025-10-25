#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/features.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

TEST(HttpHeadersCustom, ForwardsSingleAndMultipleCustomHeaders) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.statusCode(201).reason("Created");
    resp.customHeader("X-One", "1");
    resp.customHeader("X-Two", "two");
    resp.contentType("text/plain");
    resp.body("B");
    return resp;
  });
  aeronet::test::ClientConnection cc(ts.port());
  int fd = cc.fd();
  std::string req = "GET /h HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  EXPECT_TRUE(aeronet::test::sendAll(fd, req));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("201 Created"));
  ASSERT_TRUE(resp.contains("X-One: 1"));
  ASSERT_TRUE(resp.contains("X-Two: two"));
  ASSERT_TRUE(resp.contains("Content-Length: 1"));  // auto generated
  ASSERT_TRUE(resp.contains("Connection:"));        // auto generated (keep-alive or close)
}

#ifdef NDEBUG
// In release builds assertions are disabled; just ensure we can set non-reserved but not crash when attempting what
// would be reserved (we avoid actually invoking UB). This block left empty intentionally.
#else
TEST(HttpHeadersCustom, SettingReservedHeaderTriggersAssert) {
  // We use EXPECT_DEATH to verify debug assertion fires when user attempts to set reserved headers.
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  // Connection
  ASSERT_DEATH(
      {
        aeronet::HttpResponse resp;
        resp.customHeader("Connection", "keep-alive");
      },
      "");
  // Date
  ASSERT_DEATH(
      {
        aeronet::HttpResponse resp;
        resp.customHeader("Date", "Wed, 01 Jan 2020 00:00:00 GMT");
      },
      "");
  // Content-Length
  ASSERT_DEATH(
      {
        aeronet::HttpResponse resp;
        resp.customHeader("Content-Length", "10");
      },
      "");
  // Transfer-Encoding
  ASSERT_DEATH(
      {
        aeronet::HttpResponse resp;
        resp.customHeader("Transfer-Encoding", "chunked");
      },
      "");
}
#endif

TEST(HttpHeadersCustom, LocationHeaderAllowed) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp(302, "Found");
    resp.location("/new").body("");
    return resp;
  });
  aeronet::test::ClientConnection cc(ts.port());
  int fd = cc.fd();
  std::string req = "GET /h HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  EXPECT_TRUE(aeronet::test::sendAll(fd, req));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("302 Found"));
  ASSERT_TRUE(resp.contains("Location: /new"));
}

TEST(HttpHeadersCustom, CaseInsensitiveReplacementPreservesFirstCasing) {
  // Verify that calling customHeader with different casing replaces existing value without duplicating the line and
  // preserves the original header name casing from the first insertion.
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.customHeader("x-cAsE", "one");
    resp.customHeader("X-Case", "two");    // should replace value only
    resp.customHeader("X-CASE", "three");  // replace again
    resp.body("b");
    return resp;
  });
  aeronet::test::ClientConnection cc(ts.port());
  int fd = cc.fd();
  std::string req = "GET /h HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  EXPECT_TRUE(aeronet::test::sendAll(fd, req));
  std::string responseText = aeronet::test::recvUntilClosed(fd);
  // Expect only one occurrence with original first casing and final value 'three'.
  ASSERT_TRUE(responseText.contains("x-cAsE: three")) << responseText;
  EXPECT_FALSE(responseText.contains("X-Case:")) << responseText;
  EXPECT_FALSE(responseText.contains("X-CASE: three")) << responseText;
}

TEST(HttpHeadersCustom, StreamingCaseInsensitiveContentTypeAndEncodingSuppression) {
  if constexpr (!aeronet::zlibEnabled()) {
    GTEST_SKIP();
  }
  // Set up server with compression enabled; provide mixed-case Content-Type and Content-Encoding headers via writer.
  aeronet::CompressionConfig ccfg;
  ccfg.minBytes = 1;
  ccfg.preferredFormats.push_back(aeronet::Encoding::gzip);
  aeronet::HttpServerConfig scfg;
  scfg.withCompression(ccfg);
  aeronet::test::TestServer ts(std::move(scfg));
  std::string payload(128, 'Z');
  ts.server.router().setDefault([&](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
    writer.statusCode(200);
    writer.customHeader("cOnTeNt-TyPe", "text/plain");    // mixed case
    writer.customHeader("cOnTeNt-EnCoDiNg", "identity");  // should suppress auto compression
    writer.writeBody(payload.substr(0, 40));
    writer.writeBody(payload.substr(40));
    writer.end();
  });
  aeronet::test::ClientConnection cc(ts.port());
  int fd = cc.fd();
  std::string req =
      "GET /h HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  EXPECT_TRUE(aeronet::test::sendAll(fd, req));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  // Ensure our original casing appears exactly and no differently cased duplicate exists.
  ASSERT_TRUE(resp.contains("cOnTeNt-TyPe: text/plain")) << resp;
  ASSERT_TRUE(resp.contains("cOnTeNt-EnCoDiNg: identity")) << resp;
  // Should not see an added normalized Content-Type from default path.
  EXPECT_FALSE(resp.contains("Content-Type: text/plain")) << resp;
  // Body should be identity (contains long run of 'Z').
  EXPECT_TRUE(resp.contains(std::string(50, 'Z'))) << "Body appears compressed when it should not";
}

TEST(HttpHeaderTimeout, SlowHeadersConnectionClosed) {
  HttpServerConfig cfg;
  std::chrono::milliseconds readTimeout = std::chrono::milliseconds{50};
  cfg.withPort(0).withHeaderReadTimeout(readTimeout);
  // Use a short poll interval so the server's periodic maintenance (which enforces
  // header read timeouts) runs promptly even when the test runner is under heavy load.
  // This avoids flakiness when the whole test suite is executed in parallel.
  aeronet::test::TestServer ts(cfg, RouterConfig{}, std::chrono::milliseconds{5});
  ts.server.router().setDefault([](const HttpRequest&) {
    return aeronet::HttpResponse(aeronet::http::StatusCodeOK, "OK")
        .body("hi")
        .contentType(aeronet::http::ContentTypeTextPlain);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  test::ClientConnection cnx(ts.port());
  int fd = cnx.fd();
  ASSERT_GE(fd, 0) << "connect failed";
  // Send only method token slowly using test helpers
  std::string_view part1 = "GET /";  // incomplete, no version yet
  ASSERT_TRUE(aeronet::test::sendAll(fd, part1, std::chrono::milliseconds{500}));
  std::this_thread::sleep_for(readTimeout + std::chrono::milliseconds{5});
  // Attempt to finish request
  std::string_view rest = " HTTP/1.1\r\nHost: x\r\n\r\n";
  [[maybe_unused]] bool sent_ok = aeronet::test::sendAll(fd, rest, std::chrono::milliseconds{500});
  // kernel may still accept bytes, server should close shortly after detecting timeout

  // Attempt to read response; expect either empty (no response) or nothing meaningful (no 200 OK)
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string resp = aeronet::test::recvWithTimeout(fd, std::chrono::milliseconds{100});
  if (!resp.empty()) {
    // Should not have produced a 200 OK response because headers were never completed before timeout
    EXPECT_FALSE(resp.contains("200 OK")) << resp;
  }
}