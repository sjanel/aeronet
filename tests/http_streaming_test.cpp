#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;

namespace {
std::string blockingFetch(uint16_t port, const std::string& verb, const std::string& target) {
  aeronet::test::RequestOptions opt;
  opt.method = verb;
  opt.target = target;
  opt.connection = "close";  // one-shot
  auto resp = aeronet::test::request(port, opt);
  if (!resp) {
    return {};
  }
  return *resp;
}
}  // namespace

TEST(HttpStreaming, ChunkedSimple) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault(
      []([[maybe_unused]] const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& writer) {
        writer.statusCode(200);
        writer.contentType("text/plain");
        writer.writeBody("hello ");
        writer.writeBody("world");
        writer.end();
      });
  std::string resp = blockingFetch(port, "GET", "/stream");
  ts.stop();
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  // Should contain chunk sizes in hex (6 and 5) and terminating 0 chunk.
  ASSERT_TRUE(resp.contains("6\r\nhello "));
  ASSERT_TRUE(resp.contains("5\r\nworld"));
  ASSERT_TRUE(resp.contains("0\r\n\r\n"));
}

TEST(HttpStreaming, HeadSuppressedBody) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault(
      []([[maybe_unused]] const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& writer) {
        writer.statusCode(200);
        writer.contentType("text/plain");
        writer.writeBody("ignored body");  // should not be emitted for HEAD
        writer.end();
      });
  std::string resp = blockingFetch(port, "HEAD", "/head");
  ts.stop();
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  // For HEAD we expect no chunked framing. "0\r\n" alone would falsely match the Content-Length header line
  // ("Content-Length: 0\r\n"). What we really want to assert is that there is no terminating chunk sequence.
  // The terminating chunk in a chunked response would appear as "\r\n0\r\n\r\n" (preceded by the blank line
  // after headers or previous chunk). We also assert absence of Transfer-Encoding: chunked and body payload.
  ASSERT_FALSE(resp.contains("\r\n0\r\n\r\n"));
  ASSERT_FALSE(resp.contains("Transfer-Encoding: chunked"));
  ASSERT_FALSE(resp.contains("ignored body"));
  // Positive check: we do expect a Content-Length: 0 header for HEAD.
  ASSERT_TRUE(resp.contains("Content-Length: 0\r\n"));
}
