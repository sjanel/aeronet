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
#include "aeronet/test_util.hpp"
#include "test_server_fixture.hpp"

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
  TestServer ts(cfg);
  ts.server.setStreamingHandler(
      []([[maybe_unused]] const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& writer) {
        writer.statusCode(200);
        // We set Content-Length even though we write body pieces; for HEAD the body must be suppressed but CL retained.
        static constexpr std::string_view body = "abcdef";  // length 6
        writer.contentLength(body.size());
        writer.write(body.substr(0, 3));
        writer.write(body.substr(3));
        writer.end();
      });
  auto port = ts.port();
  std::string headResp;
  std::string getResp;
  raw(port, "HEAD", headResp);
  raw(port, "GET", getResp);
  ts.stop();

  ASSERT_NE(std::string::npos, headResp.find("HTTP/1.1 200"));
  ASSERT_NE(std::string::npos, headResp.find("Content-Length: 6\r\n"));
  // No chunked framing, no body.
  ASSERT_EQ(std::string::npos, headResp.find("abcdef"));
  ASSERT_EQ(std::string::npos, headResp.find("Transfer-Encoding: chunked"));
  // GET path: should carry body; since we set fixed length it should not be chunked.
  ASSERT_NE(std::string::npos, getResp.find("HTTP/1.1 200"));
  ASSERT_NE(std::string::npos, getResp.find("Content-Length: 6\r\n"));
  ASSERT_NE(std::string::npos, getResp.find("abcdef"));
  ASSERT_EQ(std::string::npos, getResp.find("Transfer-Encoding: chunked"));
}

TEST(HttpStreamingHeadContentLength, StreamingNoContentLengthUsesChunked) {
  TestServer ts(aeronet::HttpServerConfig{});
  ts.server.setStreamingHandler([](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
    writer.statusCode(200);
    writer.write("abc");
    writer.write("def");
    writer.end();
  });
  std::string getResp;
  raw(ts.port(), "GET", getResp);
  ts.stop();
  ASSERT_NE(getResp.find("HTTP/1.1 200"), std::string::npos);
  // No explicit Content-Length, chunked framing present.
  ASSERT_NE(getResp.find("Transfer-Encoding: chunked"), std::string::npos);
  ASSERT_EQ(getResp.find("Content-Length:"), std::string::npos);
  ASSERT_NE(getResp.find("abc"), std::string::npos);
  ASSERT_NE(getResp.find("def"), std::string::npos);
}

TEST(HttpStreamingHeadContentLength, StreamingLateContentLengthIgnoredStaysChunked) {
  TestServer ts(aeronet::HttpServerConfig{});
  ts.server.setStreamingHandler([](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
    writer.statusCode(200);
    writer.write("part1");
    // This should be ignored (already wrote body bytes) and we remain in chunked mode.
    writer.contentLength(9999);
    writer.write("part2");
    writer.end();
  });
  std::string getResp;
  raw(ts.port(), "GET", getResp);
  ts.stop();
  ASSERT_NE(getResp.find("HTTP/1.1 200"), std::string::npos);
  ASSERT_NE(getResp.find("Transfer-Encoding: chunked"), std::string::npos);
  // Ensure our ignored length did not appear.
  ASSERT_EQ(getResp.find("Content-Length: 9999"), std::string::npos);
  ASSERT_NE(getResp.find("part1"), std::string::npos);
  ASSERT_NE(getResp.find("part2"), std::string::npos);
}

#if AERONET_ENABLE_ZLIB
TEST(HttpStreamingHeadContentLength, StreamingContentLengthWithAutoCompressionDiscouragedButHonored) {
  // We intentionally (mis)use contentLength with auto compression; library will not adjust size.
  aeronet::CompressionConfig cc;
  cc.minBytes = 1;  // ensure immediate activation
  aeronet::HttpServerConfig cfg;
  cfg.withCompression(cc);
  TestServer ts(cfg);
  static constexpr std::string_view kBody =
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";  // 64 'A'
  const std::size_t originalSize = kBody.size();
  ts.server.setStreamingHandler([&](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
    writer.statusCode(200);
    writer.contentLength(originalSize);  // declares uncompressed length
    writer.write(kBody.substr(0, 10));
    writer.write(kBody.substr(10));
    writer.end();
  });
  std::string resp;
  rawWith(ts.port(), "GET", "Accept-Encoding: gzip\r\n", resp);
  ts.stop();
  ASSERT_NE(resp.find("HTTP/1.1 200"), std::string::npos);
  // We expect a fixed-length header present.
  std::string clHeader = std::string("Content-Length: ") + std::to_string(originalSize) + "\r\n";
  ASSERT_NE(resp.find(clHeader), std::string::npos);
  // Compression should have activated producing a gzip header (1F 8B in hex) and Content-Encoding header.
  ASSERT_NE(resp.find("Content-Encoding: gzip"), std::string::npos);
  // Body should not be chunked.
  ASSERT_EQ(resp.find("Transfer-Encoding: chunked"), std::string::npos);
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
