#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

#include "aeronet/async-http-server.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "file.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

namespace {
std::string blockingFetch(uint16_t port, const std::string& verb, const std::string& target) {
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

std::string doRequest(auto port, std::string_view verb, std::string_view target) {
  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req(verb);
  req.push_back(' ');
  req.append(target);

  req.append(" HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
  test::sendAll(fd, req);
  return test::recvUntilClosed(fd);
}

std::string httpRequest(auto port, std::string_view method, std::string_view path, const std::string& body = {}) {
  test::ClientConnection cnx(port);
  int fd = cnx.fd();

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
std::string extractBody(const std::string& resp) {
  auto headerEnd = resp.find(http::DoubleCRLF);
  if (headerEnd == std::string::npos) {
    return {};
  }
  std::string body = resp.substr(headerEnd + http::DoubleCRLF.size());
  // If not chunked just return remaining.
  if (!body.contains("\r\n0\r\n") && !body.contains("0\r\n\r\n")) {
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
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([]([[maybe_unused]] const HttpRequest& req, HttpResponseWriter& writer) {
    writer.status(200);
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

TEST(HttpStreaming, SendFileFixedLengthPlain) {
  constexpr std::string_view kPayload = "static sendfile response body";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);

  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  std::string path = tmp.filePath().string();

  ts.server.router().setDefault([path](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(200);
    writer.file(File(path));
    writer.end();
  });

  std::string resp = blockingFetch(port, "GET", "/file");
  ts.stop();

  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  ASSERT_FALSE(resp.contains("Transfer-Encoding: chunked"));
  ASSERT_TRUE(resp.contains("Content-Length: " + std::to_string(kPayload.size())));

  auto headerEnd = resp.find(http::DoubleCRLF);
  ASSERT_NE(std::string::npos, headerEnd);
  std::string body = resp.substr(headerEnd + http::DoubleCRLF.size());
  EXPECT_EQ(body, kPayload);
}

TEST(HttpStreaming, SendFileHeadSuppressesBody) {
  constexpr std::string_view kPayload = "head sendfile streaming";
  test::ScopedTempDir tmpDir;
  test::ScopedTempFile tmp(tmpDir, kPayload);

  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  std::string path = tmp.filePath().string();

  ts.server.router().setDefault([path](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(200);
    writer.file(File(path));
    writer.end();
  });

  std::string resp = blockingFetch(port, "HEAD", "/file");
  ts.stop();

  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("Content-Length: " + std::to_string(kPayload.size())));
  ASSERT_FALSE(resp.contains("Transfer-Encoding: chunked"));

  auto headerEnd = resp.find(http::DoubleCRLF);
  ASSERT_NE(std::string::npos, headerEnd);
  std::string body = resp.substr(headerEnd + http::DoubleCRLF.size());
  EXPECT_TRUE(body.empty());
}

TEST(HttpStreaming, HeadSuppressedBody) {
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([]([[maybe_unused]] const HttpRequest& req, HttpResponseWriter& writer) {
    writer.status(200);
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

// Coverage goals:
// 1. setHeader emits custom headers.
// 2. Multiple calls with unique names all appear.
// 3. Overriding Content-Type via setHeader before any body suppresses default text/plain.
// 4. Calling setHeader after headers were implicitly sent (by first write) has no effect.
// 5. HEAD request: headers still emitted correctly without body/chunk framing; Content-Length auto added when absent.

TEST(HttpStreamingSetHeader, MultipleCustomHeadersAndOverrideContentType) {
  test::TestServer ts(HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([]([[maybe_unused]] const HttpRequest& req, HttpResponseWriter& writer) {
    writer.status(200);
    writer.header("X-Custom-A", "alpha");
    writer.header("X-Custom-B", "beta");
    writer.header("Content-Type", "application/json");  // override default
    // First write sends headers implicitly.
    writer.writeBody("{\"k\":1}");
    // These should be ignored because headers already sent.
    writer.header("X-Ignored", "zzz");
    writer.header("Content-Type", "text/plain");
    writer.end();
  });

  std::string getResp = doRequest(port, "GET", "/hdr");
  std::string headResp = doRequest(port, "HEAD", "/hdr");

  // Basic status line check
  ASSERT_TRUE(getResp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(headResp.contains("HTTP/1.1 200"));
  // Custom headers should appear exactly once each.
  ASSERT_TRUE(getResp.contains("X-Custom-A: alpha\r\n"));
  ASSERT_TRUE(getResp.contains("X-Custom-B: beta\r\n"));
  // Overridden content type
  ASSERT_TRUE(getResp.contains("Content-Type: application/json\r\n"));
  // Default text/plain should not appear.
  ASSERT_FALSE(getResp.contains("Content-Type: text/plain"));
  // Ignored header should not appear.
  ASSERT_FALSE(getResp.contains("X-Ignored: zzz"));
  // Body present in GET but not in HEAD.
  ASSERT_TRUE(getResp.contains("{\"k\":1}"));
  ASSERT_FALSE(headResp.contains("{\"k\":1}"));
  // HEAD: ensure Content-Length auto added (0 since body suppressed) and no chunk framing.
  ASSERT_TRUE(headResp.contains("Content-Length: 0\r\n"));
  ASSERT_FALSE(headResp.contains("Transfer-Encoding: chunked"));
}

TEST(HttpServerMixed, MixedPerPathHandlers) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = false;
  HttpServer srv(cfg);
  // path /mix : GET streaming, POST normal
  srv.router().setPath(http::Method::GET, "/mix", [](const HttpRequest& /*unused*/, HttpResponseWriter& writer) {
    writer.status(200);
    writer.header("Content-Type", "text/plain");
    writer.writeBody("S");
    writer.writeBody("TREAM");
    writer.end();
  });
  srv.router().setPath(http::Method::POST, "/mix", [](const HttpRequest& /*unused*/) {
    return HttpResponse(201).reason("Created").body("NORMAL");
  });
  std::jthread th([&] { srv.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string getResp = httpRequest(srv.port(), "GET", "/mix");
  auto decoded = extractBody(getResp);
  EXPECT_EQ(decoded, "STREAM");
  std::string postResp = httpRequest(srv.port(), "POST", "/mix", "x");
  EXPECT_TRUE(postResp.contains("NORMAL"));
  srv.stop();
}

TEST(HttpServerMixed, ConflictRegistrationNormalThenStreaming) {
  HttpServerConfig cfg;
  HttpServer srv(cfg);
  srv.router().setPath(http::Method::GET, "/c", [](const HttpRequest&) { return HttpResponse(200, "OK").body("X"); });
  EXPECT_THROW(srv.router().setPath(http::Method::GET, "/c", [](const HttpRequest&, HttpResponseWriter&) {}),
               std::logic_error);
}

TEST(HttpServerMixed, ConflictRegistrationStreamingThenNormal) {
  HttpServerConfig cfg;
  HttpServer srv(cfg);
  srv.router().setPath(http::Method::GET, "/c2", [](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(200);
    writer.end();
  });
  EXPECT_THROW(srv.router().setPath(http::Method::GET, "/c2",
                                    [](const HttpRequest&) { return HttpResponse(200, "OK").body("Y"); }),
               std::logic_error);
}

TEST(HttpServerMixed, GlobalFallbackPrecedence) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = false;
  HttpServer srv(cfg);
  srv.router().setDefault([](const HttpRequest&) { return HttpResponse(200, "OK").body("GLOBAL"); });
  srv.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(200);
    writer.header("Content-Type", "text/plain");
    writer.writeBody("STREAMFALLBACK");
    writer.end();
  });
  // path-specific streaming overrides both
  srv.router().setPath(http::Method::GET, "/s", [](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(200);
    writer.writeBody("PS");
    writer.end();
  });
  // path-specific normal overrides global fallbacks
  srv.router().setPath(http::Method::GET, "/n",
                       [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, "OK").body("PN"); });
  std::jthread th([&] { srv.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string pathStreamResp = httpRequest(srv.port(), "GET", "/s");
  EXPECT_TRUE(pathStreamResp.contains("PS"));
  std::string pathNormalResp = httpRequest(srv.port(), "GET", "/n");
  EXPECT_TRUE(pathNormalResp.contains("PN"));
  std::string fallback = httpRequest(srv.port(), "GET", "/other");
  // Should use global streaming first (higher precedence than global normal)
  EXPECT_TRUE(fallback.contains("STREAMFALLBACK"));
  srv.stop();
}

TEST(HttpServerMixed, GlobalNormalOnlyWhenNoStreaming) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = false;
  HttpServer srv(cfg);
  srv.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, "OK").body("GN"); });
  std::jthread th([&] { srv.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  std::string result = httpRequest(srv.port(), "GET", "/x");
  EXPECT_TRUE(result.contains("GN"));
  srv.stop();
}

TEST(HttpServerMixed, HeadRequestOnStreamingPathSuppressesBody) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = false;
  HttpServer srv(cfg);
  // Register streaming handler for GET; it will attempt to write a body.
  srv.router().setPath(http::Method::GET, "/head", [](const HttpRequest& /*unused*/, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.header("Content-Type", "text/plain");
    writer.writeBody("SHOULD_NOT_APPEAR");  // for HEAD this must be suppressed by writer
    writer.end();
  });
  std::jthread th([&] { srv.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string headResp = httpRequest(srv.port(), "HEAD", "/head");
  // Body should be empty; ensure word not present and Content-Length: 0 (or if chunked not used at all)
  auto headerEnd = headResp.find(http::DoubleCRLF);
  ASSERT_NE(std::string::npos, headerEnd);
  std::string bodyPart = headResp.substr(headerEnd + 4);
  EXPECT_TRUE(bodyPart.empty());
  // Either explicit Content-Length: 0 is present or (future) alternate header; assert current behavior.
  EXPECT_TRUE(headResp.contains("Content-Length: 0"));
  EXPECT_FALSE(headResp.contains("SHOULD_NOT_APPEAR"));
  srv.stop();
}

TEST(HttpServerMixed, MethodNotAllowedWhenOnlyOtherStreamingMethodRegistered) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = false;
  HttpServer srv(cfg);
  // Register only GET streaming handler
  srv.router().setPath(http::Method::GET, "/m405", [](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("OKGET");
    writer.end();
  });
  std::jthread th([&] { srv.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string postResp = httpRequest(srv.port(), "POST", "/m405", "data");
  // Expect 405 Method Not Allowed
  EXPECT_TRUE(postResp.contains("405"));
  EXPECT_TRUE(postResp.contains("Method Not Allowed"));
  // Ensure GET still works and returns streaming body
  std::string getResp2 = httpRequest(srv.port(), "GET", "/m405");
  auto decoded2 = extractBody(getResp2);
  EXPECT_EQ(decoded2, "OKGET");
  srv.stop();
}

TEST(HttpServerMixed, KeepAliveSequentialMixedStreamingAndNormal) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  cfg.maxRequestsPerConnection = 3;  // allow at least two
  HttpServer srv(cfg);
  // Register streaming GET and normal POST on same path
  srv.router().setPath(http::Method::GET, "/ka", [](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.header("Content-Type", "text/plain");
    writer.writeBody("A");
    writer.writeBody("B");
    writer.end();
  });
  srv.router().setPath(http::Method::POST, "/ka",
                       [](const HttpRequest&) { return HttpResponse(201).reason("Created").body("NORMAL"); });
  std::jthread th([&] { srv.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  // Build raw requests (each must include Host and Connection: keep-alive)
  std::string r1 = "GET /ka HTTP/1.1\r\nHost: test\r\nConnection: keep-alive\r\n\r\n";  // streaming
  std::string r2 =
      "POST /ka HTTP/1.1\r\nHost: test\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";  // normal, closes

  test::ClientConnection cnx(srv.port());

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
  auto body1 = extractBody(firstResponse);
  EXPECT_EQ(body1, "AB");
  // Second response should have NORMAL
  auto secondBodyStart = raw.find("NORMAL", afterFirst);
  EXPECT_NE(std::string::npos, secondBodyStart);
  srv.stop();
}

TEST(StreamingKeepAlive, TwoSequentialRequests) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  cfg.pollInterval = std::chrono::milliseconds(5);
  AsyncHttpServer server(cfg);
  server.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("hello");
    writer.writeBody(",world");
    writer.end();
  });

  server.start();

  auto port = server.port();
  ASSERT_NE(port, 0);

  test::ClientConnection cnx(port);
  int fd = cnx.fd();
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
  HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  cfg.pollInterval = std::chrono::milliseconds(5);
  AsyncHttpServer server(cfg);
  server.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("ignored-body");
    writer.end();
  });
  server.start();

  auto port = server.port();
  ASSERT_GT(port, 0);
  test::ClientConnection cnx(port);
  int fd = cnx.fd();

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
void raw(auto port, const std::string& verb, std::string& out) {
  test::ClientConnection sock(port);
  int fd = sock.fd();
  std::string req = verb + " /len HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  out = test::recvUntilClosed(fd);
}

void rawWith(auto port, const std::string& verb, std::string_view extraHeaders, std::string& out) {
  test::ClientConnection sock(port);
  int fd = sock.fd();
  std::string req = verb + " /len HTTP/1.1\r\nHost: x\r\n" + std::string(extraHeaders) + "Connection: close\r\n\r\n";
  test::sendAll(fd, req);
  out = test::recvUntilClosed(fd);
}
}  // namespace

TEST(HttpStreamingHeadContentLength, HeadSuppressesBodyKeepsCL) {
  HttpServerConfig cfg;
  cfg.withMaxRequestsPerConnection(2);
  test::TestServer ts(cfg);
  ts.server.router().setDefault([]([[maybe_unused]] const HttpRequest& req, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
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
  test::TestServer ts(HttpServerConfig{});
  ts.server.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
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
  test::TestServer ts(HttpServerConfig{});
  ts.server.router().setDefault([](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
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
  CompressionConfig cc;
  cc.minBytes = 1;  // ensure immediate activation
  HttpServerConfig cfg;
  cfg.withCompression(cc);
  test::TestServer ts(cfg);
  static constexpr std::string_view kBody =
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";  // 64 'A'
  const std::size_t originalSize = kBody.size();
  ts.server.router().setDefault([&](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.contentLength(originalSize);  // declares uncompressed length
    writer.writeBody(kBody.substr(0, 10));
    writer.writeBody(kBody.substr(10));
    writer.end();
  });
  std::string resp;
  rawWith(ts.port(), "GET", "Accept-Encoding: gzip\r\n", resp);
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  // We expect a fixed-length header present.
  std::string clHeader = std::string("Content-Length: ") + std::to_string(originalSize) + "\r\n";
  ASSERT_TRUE(resp.contains(clHeader));
  // Compression should have activated producing a gzip header (1F 8B in hex) and Content-Encoding header.
  ASSERT_TRUE(resp.contains("Content-Encoding: gzip"));
  // Body should not be chunked.
  ASSERT_FALSE(resp.contains("Transfer-Encoding: chunked"));
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
  HttpServerConfig cfg;
  cfg.enableKeepAlive = false;                                       // simplicity
  cfg.maxOutboundBufferBytes = static_cast<std::size_t>(64 * 1024);  // assume default maybe larger
  test::TestServer ts(cfg);
  std::size_t total = static_cast<std::size_t>(512 * 1024);  // 512 KB
  ts.server.router().setDefault([&]([[maybe_unused]] const HttpRequest& req, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    std::string chunk(8192, 'x');
    std::size_t sent = 0;
    while (sent < total) {
      writer.writeBody(chunk);
      sent += chunk.size();
    }
    writer.end();
  });
  auto port = ts.port();
  test::ClientConnection cnx(port);
  int fd = cnx.fd();
  std::string req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
  test::sendAll(fd, req);

  auto data = test::recvUntilClosed(fd);
  EXPECT_TRUE(data.starts_with("HTTP/1.1 200"));
}

TEST(HttpStreamingAdaptive, CoalescedAndLargePaths) {
  constexpr std::size_t kLargeSize = 5000;

  HttpServerConfig cfg;
  cfg.minCapturedBodySize = kLargeSize - 1U;
  test::TestServer ts(cfg);
  auto port = ts.port();
  std::string large(kLargeSize, 'x');
  ts.server.router().setDefault([&](const HttpRequest&, HttpResponseWriter& writer) {
    writer.status(http::StatusCodeOK);
    writer.writeBody("small");  // coalesced path
    writer.writeBody(large);    // large path (multi enqueue)
    writer.end();
  });
  std::string resp = blockingFetch(port, "GET", "/adaptive");
  auto stats = ts.server.stats();
  EXPECT_GT(stats.totalBytesWrittenImmediate, kLargeSize);
  ts.stop();
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
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
