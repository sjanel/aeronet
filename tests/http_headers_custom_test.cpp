#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "aeronet/compression-config.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

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
  ASSERT_NE(std::string::npos, resp.find("201 Created"));
  ASSERT_NE(std::string::npos, resp.find("X-One: 1"));
  ASSERT_NE(std::string::npos, resp.find("X-Two: two"));
  ASSERT_NE(std::string::npos, resp.find("Content-Length: 1"));  // auto generated
  ASSERT_NE(std::string::npos, resp.find("Connection:"));        // auto generated (keep-alive or close)
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
  ASSERT_NE(std::string::npos, resp.find("302 Found"));
  ASSERT_NE(std::string::npos, resp.find("Location: /new"));
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
  ASSERT_NE(responseText.find("x-cAsE: three"), std::string::npos) << responseText;
  EXPECT_EQ(responseText.find("X-Case:"), std::string::npos) << responseText;
  EXPECT_EQ(responseText.find("X-CASE: three"), std::string::npos) << responseText;
}

#if AERONET_ENABLE_ZLIB
TEST(HttpHeadersCustom, StreamingCaseInsensitiveContentTypeAndEncodingSuppression) {
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
    writer.write(payload.substr(0, 40));
    writer.write(payload.substr(40));
    writer.end();
  });
  aeronet::test::ClientConnection cc(ts.port());
  int fd = cc.fd();
  std::string req =
      "GET /h HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  EXPECT_TRUE(aeronet::test::sendAll(fd, req));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  // Ensure our original casing appears exactly and no differently cased duplicate exists.
  ASSERT_NE(resp.find("cOnTeNt-TyPe: text/plain"), std::string::npos) << resp;
  ASSERT_NE(resp.find("cOnTeNt-EnCoDiNg: identity"), std::string::npos) << resp;
  // Should not see an added normalized Content-Type from default path.
  EXPECT_EQ(resp.find("Content-Type: text/plain"), std::string::npos) << resp;
  // Body should be identity (contains long run of 'Z').
  EXPECT_NE(resp.find(std::string(50, 'Z')), std::string::npos) << "Body appears compressed when it should not";
}
#endif
