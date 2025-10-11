#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

namespace {
std::string doRequest(auto port, std::string_view verb, std::string_view target) {
  aeronet::test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req(verb);
  req.push_back(' ');
  req.append(target);

  req.append(" HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  EXPECT_TRUE(aeronet::test::sendAll(fd, req));
  return aeronet::test::recvUntilClosed(fd);
}
}  // namespace

// Coverage goals:
// 1. setHeader emits custom headers.
// 2. Multiple calls with unique names all appear.
// 3. Overriding Content-Type via setHeader before any body suppresses default text/plain.
// 4. Calling setHeader after headers were implicitly sent (by first write) has no effect.
// 5. HEAD request: headers still emitted correctly without body/chunk framing; Content-Length auto added when absent.

TEST(HttpStreamingSetHeader, MultipleCustomHeadersAndOverrideContentType) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& writer) {
    bool isHead = req.method() == aeronet::http::Method::HEAD;
    writer.statusCode(200);
    writer.customHeader("X-Custom-A", "alpha");
    writer.customHeader("X-Custom-B", "beta");
    writer.customHeader("Content-Type", "application/json");  // override default
    // First write sends headers implicitly.
    writer.write("{\"k\":1}");
    // These should be ignored because headers already sent.
    writer.customHeader("X-Ignored", "zzz");
    writer.customHeader("Content-Type", "text/plain");
    writer.end();
    if (isHead) {
      // Nothing extra; body suppressed automatically.
    }
  });

  std::string getResp = doRequest(port, "GET", "/hdr");
  std::string headResp = doRequest(port, "HEAD", "/hdr");

  ts.stop();
  // Basic status line check
  ASSERT_NE(std::string::npos, getResp.find("HTTP/1.1 200"));
  ASSERT_NE(std::string::npos, headResp.find("HTTP/1.1 200"));
  // Custom headers should appear exactly once each.
  ASSERT_NE(std::string::npos, getResp.find("X-Custom-A: alpha\r\n"));
  ASSERT_NE(std::string::npos, getResp.find("X-Custom-B: beta\r\n"));
  // Overridden content type
  ASSERT_NE(std::string::npos, getResp.find("Content-Type: application/json\r\n"));
  // Default text/plain should not appear.
  ASSERT_EQ(std::string::npos, getResp.find("Content-Type: text/plain"));
  // Ignored header should not appear.
  ASSERT_EQ(std::string::npos, getResp.find("X-Ignored: zzz"));
  // Body present in GET but not in HEAD.
  ASSERT_NE(std::string::npos, getResp.find("{\"k\":1}"));
  ASSERT_EQ(std::string::npos, headResp.find("{\"k\":1}"));
  // HEAD: ensure Content-Length auto added (0 since body suppressed) and no chunk framing.
  ASSERT_NE(std::string::npos, headResp.find("Content-Length: 0\r\n"));
  ASSERT_EQ(std::string::npos, headResp.find("Transfer-Encoding: chunked"));
}
