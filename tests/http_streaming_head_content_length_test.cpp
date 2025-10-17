#include <gtest/gtest.h>
#include <sys/socket.h>

#include <cstddef>  // std::size_t
#include <string>
#include <string_view>

#include "aeronet/compression-config.hpp"  // aeronet::CompressionConfig (zlib test section)
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;

namespace {
void raw(auto port, const std::string& verb, std::string& out) {
  aeronet::test::ClientConnection sock(port);
  int fd = sock.fd();
  std::string req = verb + " /len HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  auto sent = ::send(fd, req.data(), req.size(), 0);
  ASSERT_EQ(sent, static_cast<decltype(sent)>(req.size())) << "send partial";
  char buf[4096];
  out.clear();
  while (true) {
    auto bytesRead = ::recv(fd, buf, sizeof(buf), 0);
    if (bytesRead <= 0) {
      break;
    }
    out.append(buf, buf + bytesRead);
  }
}

void rawWith(auto port, const std::string& verb, std::string_view extraHeaders, std::string& out) {
  aeronet::test::ClientConnection sock(port);
  int fd = sock.fd();
  std::string req = verb + " /len HTTP/1.1\r\nHost: x\r\n" + std::string(extraHeaders) + "Connection: close\r\n\r\n";
  auto sent = ::send(fd, req.data(), req.size(), 0);
  ASSERT_EQ(sent, static_cast<decltype(sent)>(req.size())) << "send partial";
  char buf[4096];
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

TEST(HttpStreamingHeadContentLength, HeadSuppressesBodyKeepsCL) {
  aeronet::HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(2);
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setDefault(
      []([[maybe_unused]] const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& writer) {
        writer.statusCode(200);
        // We set Content-Length even though we write body pieces; for HEAD the body must be suppressed but CL retained.
        static constexpr std::string_view body = "abcdef";  // length 6
        writer.contentLength(body.size());
        writer.writeBody(body.substr(0, 3));
        writer.writeBody(body.substr(3));
        writer.end();
      });
  auto port = ts.port();
  std::string headResp;
  std::string getResp;
  raw(port, "HEAD", headResp);
  raw(port, "GET", getResp);
  ts.stop();

  ASSERT_TRUE(headResp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(headResp.contains("Content-Length: 6\r\n"));
  // No chunked framing, no body.
  ASSERT_FALSE(headResp.contains("abcdef"));
  ASSERT_FALSE(headResp.contains("Transfer-Encoding: chunked"));
  // GET path: should carry body; since we set fixed length it should not be chunked.
  ASSERT_TRUE(getResp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(getResp.contains("Content-Length: 6\r\n"));
  ASSERT_TRUE(getResp.contains("abcdef"));
  ASSERT_FALSE(getResp.contains("Transfer-Encoding: chunked"));
}

TEST(HttpStreamingHeadContentLength, StreamingNoContentLengthUsesChunked) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
    writer.statusCode(200);
    writer.writeBody("abc");
    writer.writeBody("def");
    writer.end();
  });
  std::string getResp;
  raw(ts.port(), "GET", getResp);
  ts.stop();
  ASSERT_TRUE(getResp.contains("HTTP/1.1 200"));
  // No explicit Content-Length, chunked framing present.
  ASSERT_TRUE(getResp.contains("Transfer-Encoding: chunked"));
  ASSERT_FALSE(getResp.contains("Content-Length:"));
  ASSERT_TRUE(getResp.contains("abc"));
  ASSERT_TRUE(getResp.contains("def"));
}

TEST(HttpStreamingHeadContentLength, StreamingLateContentLengthIgnoredStaysChunked) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
    writer.statusCode(200);
    writer.writeBody("part1");
    // This should be ignored (already wrote body bytes) and we remain in chunked mode.
    writer.contentLength(9999);
    writer.writeBody("part2");
    writer.end();
  });
  std::string getResp;
  raw(ts.port(), "GET", getResp);
  ts.stop();
  ASSERT_TRUE(getResp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(getResp.contains("Transfer-Encoding: chunked"));
  // Ensure our ignored length did not appear.
  ASSERT_FALSE(getResp.contains("Content-Length: 9999"));
  ASSERT_TRUE(getResp.contains("part1"));
  ASSERT_TRUE(getResp.contains("part2"));
}

#if AERONET_ENABLE_ZLIB
TEST(HttpStreamingHeadContentLength, StreamingContentLengthWithAutoCompressionDiscouragedButHonored) {
  // We intentionally (mis)use contentLength with auto compression; library will not adjust size.
  aeronet::CompressionConfig cc;
  cc.minBytes = 1;  // ensure immediate activation
  aeronet::HttpServerConfig cfg;
  cfg.withCompression(cc);
  aeronet::test::TestServer ts(cfg);
  static constexpr std::string_view kBody =
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";  // 64 'A'
  const std::size_t originalSize = kBody.size();
  ts.server.router().setDefault([&](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
    writer.statusCode(200);
    writer.contentLength(originalSize);  // declares uncompressed length
    writer.writeBody(kBody.substr(0, 10));
    writer.writeBody(kBody.substr(10));
    writer.end();
  });
  std::string resp;
  rawWith(ts.port(), "GET", "Accept-Encoding: gzip\r\n", resp);
  ts.stop();
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  // We expect a fixed-length header present.
  std::string clHeader = std::string("Content-Length: ") + std::to_string(originalSize) + "\r\n";
  ASSERT_TRUE(resp.contains(clHeader));
  // Compression should have activated producing a gzip header (1F 8B in hex) and Content-Encoding header.
  ASSERT_TRUE(resp.contains("Content-Encoding: gzip"));
  // Body should not be chunked.
  ASSERT_FALSE(resp.contains("Transfer-Encoding: chunked"));
  // Extract body (after double CRLF) and verify it differs from original (compressed) and starts with gzip magic.
  auto pos = resp.find("\r\n\r\n");
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
