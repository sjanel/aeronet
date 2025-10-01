#include <gtest/gtest.h>
#include <sys/socket.h>

#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"
#include "http-method.hpp"
#include "test_server_fixture.hpp"

namespace {
void doRequest(auto port, const std::string& verb, const std::string& target, std::string& out) {
  aeronet::test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req = verb + " " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  auto sent = ::send(fd, req.data(), req.size(), 0);
  ASSERT_EQ(sent, static_cast<decltype(sent)>(req.size())) << "send partial";
  char buf[8192];
  out.clear();
  while (true) {
    auto bytesRead = ::recv(fd, buf, sizeof(buf), 0);
    if (bytesRead <= 0) {
      break;
    }
    out.append(buf, buf + bytesRead);
  }
}
}  // namespace

// Coverage goals:
// 1. setHeader emits custom headers.
// 2. Multiple calls with unique names all appear.
// 3. Overriding Content-Type via setHeader before any body suppresses default text/plain.
// 4. Calling setHeader after headers were implicitly sent (by first write) has no effect.
// 5. HEAD request: headers still emitted correctly without body/chunk framing; Content-Length auto added when absent.

TEST(HttpStreamingSetHeader, MultipleCustomHeadersAndOverrideContentType) {
  TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.setStreamingHandler([](const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& writer) {
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

  std::string getResp;
  std::string headResp;
  doRequest(port, "GET", "/hdr", getResp);
  doRequest(port, "HEAD", "/hdr", headResp);
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
