#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "test_http_client.hpp"
#include "test_server_fixture.hpp"

using namespace std::chrono_literals;

namespace {
std::string blockingFetch(uint16_t port, const std::string& verb, const std::string& target) {
  test_http_client::RequestOptions opt;
  opt.method = verb;
  opt.target = target;
  opt.connection = "close";  // one-shot
  auto resp = test_http_client::request(port, opt);
  if (!resp) {
    return {};
  }
  return *resp;
}
}  // namespace

TEST(HttpStreaming, ChunkedSimple) {
  TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.setStreamingHandler(
      []([[maybe_unused]] const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& writer) {
        writer.setStatus(200, "OK");
        writer.setContentType("text/plain");
        writer.write("hello ");
        writer.write("world");
        writer.end();
      });
  std::string resp = blockingFetch(port, "GET", "/stream");
  ts.stop();
  ASSERT_NE(std::string::npos, resp.find("HTTP/1.1 200"));
  // Should contain chunk sizes in hex (6 and 5) and terminating 0 chunk.
  ASSERT_NE(std::string::npos, resp.find("6\r\nhello "));
  ASSERT_NE(std::string::npos, resp.find("5\r\nworld"));
  ASSERT_NE(std::string::npos, resp.find("0\r\n\r\n"));
}

TEST(HttpStreaming, HeadSuppressedBody) {
  TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.setStreamingHandler(
      []([[maybe_unused]] const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& writer) {
        writer.setStatus(200, "OK");
        writer.setContentType("text/plain");
        writer.write("ignored body");  // should not be emitted for HEAD
        writer.end();
      });
  std::string resp = blockingFetch(port, "HEAD", "/head");
  ts.stop();
  ASSERT_NE(std::string::npos, resp.find("HTTP/1.1 200"));
  // For HEAD we expect no chunked framing. "0\r\n" alone would falsely match the Content-Length header line
  // ("Content-Length: 0\r\n"). What we really want to assert is that there is no terminating chunk sequence.
  // The terminating chunk in a chunked response would appear as "\r\n0\r\n\r\n" (preceded by the blank line
  // after headers or previous chunk). We also assert absence of Transfer-Encoding: chunked and body payload.
  ASSERT_EQ(std::string::npos, resp.find("\r\n0\r\n\r\n"));
  ASSERT_EQ(std::string::npos, resp.find("Transfer-Encoding: chunked"));
  ASSERT_EQ(std::string::npos, resp.find("ignored body"));
  // Positive check: we do expect a Content-Length: 0 header for HEAD.
  ASSERT_NE(std::string::npos, resp.find("Content-Length: 0\r\n"));
}
