#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"
#include "exception.hpp"

namespace {
std::string httpRequest(auto port, std::string_view method, std::string_view path, const std::string& body = {}) {
  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();

  std::string req = std::string(method) + " " + std::string(path) + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n";
  if (!body.empty()) {
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  req += "\r\n";
  req += body;

  aeronet::test::sendAll(fd, req);
  return aeronet::test::recvUntilClosed(fd);
}

// Very small chunked decoder for tests (single pass, no trailers). Expects full HTTP response.
std::string extractBody(const std::string& resp) {
  auto headerEnd = resp.find(aeronet::http::DoubleCRLF);
  if (headerEnd == std::string::npos) {
    return {};
  }
  std::string body = resp.substr(headerEnd + aeronet::http::DoubleCRLF.size());
  // If not chunked just return remaining.
  if (body.find("\r\n0\r\n") == std::string::npos && body.find("0\r\n\r\n") == std::string::npos) {
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

TEST(HttpServerMixed, MixedPerPathHandlers) {
  aeronet::HttpServerConfig cfg;
  cfg.enableKeepAlive = false;
  aeronet::HttpServer srv(cfg);
  // path /mix : GET streaming, POST normal
  srv.router().setPath("/mix", aeronet::http::Method::GET,
                       [](const aeronet::HttpRequest& /*unused*/, aeronet::HttpResponseWriter& writer) {
                         writer.statusCode(200);
                         writer.customHeader("Content-Type", "text/plain");
                         writer.write("S");
                         writer.write("TREAM");
                         writer.end();
                       });
  srv.router().setPath("/mix", aeronet::http::Method::POST, [](const aeronet::HttpRequest& /*unused*/) {
    return aeronet::HttpResponse(201).reason("Created").contentType(aeronet::http::ContentTypeTextPlain).body("NORMAL");
  });
  std::jthread th([&] { srv.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string getResp = httpRequest(srv.port(), "GET", "/mix");
  auto decoded = extractBody(getResp);
  EXPECT_EQ(decoded, "STREAM");
  std::string postResp = httpRequest(srv.port(), "POST", "/mix", "x");
  EXPECT_NE(std::string::npos, postResp.find("NORMAL"));
  srv.stop();
}

TEST(HttpServerMixed, ConflictRegistrationNormalThenStreaming) {
  aeronet::HttpServerConfig cfg;
  aeronet::HttpServer srv(cfg);
  srv.router().setPath("/c", aeronet::http::Method::GET, [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse(200, "OK").body("X").contentType("text/plain");
  });
  EXPECT_THROW(srv.router().setPath("/c", aeronet::http::Method::GET,
                                    [](const aeronet::HttpRequest&, aeronet::HttpResponseWriter&) {}),
               aeronet::exception);
}

TEST(HttpServerMixed, ConflictRegistrationStreamingThenNormal) {
  aeronet::HttpServerConfig cfg;
  aeronet::HttpServer srv(cfg);
  srv.router().setPath("/c2", aeronet::http::Method::GET,
                       [](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
                         writer.statusCode(200);
                         writer.end();
                       });
  EXPECT_THROW(srv.router().setPath("/c2", aeronet::http::Method::GET,
                                    [](const aeronet::HttpRequest&) {
                                      return aeronet::HttpResponse(200, "OK").body("Y").contentType("text/plain");
                                    }),
               aeronet::exception);
}

TEST(HttpServerMixed, GlobalFallbackPrecedence) {
  aeronet::HttpServerConfig cfg;
  cfg.enableKeepAlive = false;
  aeronet::HttpServer srv(cfg);
  srv.router().setDefault([](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse(200, "OK").contentType(aeronet::http::ContentTypeTextPlain).body("GLOBAL");
  });
  srv.router().setDefault([](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
    writer.statusCode(200);
    writer.customHeader("Content-Type", "text/plain");
    writer.write("STREAMFALLBACK");
    writer.end();
  });
  // path-specific streaming overrides both
  srv.router().setPath("/s", aeronet::http::Method::GET,
                       [](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
                         writer.statusCode(200);
                         writer.write("PS");
                         writer.end();
                       });
  // path-specific normal overrides global fallbacks
  srv.router().setPath("/n", aeronet::http::Method::GET, [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse(200, "OK").body("PN").contentType("text/plain");
  });
  std::jthread th([&] { srv.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string pathStreamResp = httpRequest(srv.port(), "GET", "/s");
  EXPECT_NE(std::string::npos, pathStreamResp.find("PS"));
  std::string pathNormalResp = httpRequest(srv.port(), "GET", "/n");
  EXPECT_NE(std::string::npos, pathNormalResp.find("PN"));
  std::string fallback = httpRequest(srv.port(), "GET", "/other");
  // Should use global streaming first (higher precedence than global normal)
  EXPECT_NE(std::string::npos, fallback.find("STREAMFALLBACK"));
  srv.stop();
}

TEST(HttpServerMixed, GlobalNormalOnlyWhenNoStreaming) {
  aeronet::HttpServerConfig cfg;
  cfg.enableKeepAlive = false;
  aeronet::HttpServer srv(cfg);
  srv.router().setDefault([](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse(200, "OK").body("GN").contentType("text/plain");
  });
  std::jthread th([&] { srv.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  std::string result = httpRequest(srv.port(), "GET", "/x");
  EXPECT_NE(std::string::npos, result.find("GN"));
  srv.stop();
}

TEST(HttpServerMixed, HeadRequestOnStreamingPathSuppressesBody) {
  aeronet::HttpServerConfig cfg;
  cfg.enableKeepAlive = false;
  aeronet::HttpServer srv(cfg);
  // Register streaming handler for GET; it will attempt to write a body.
  srv.router().setPath("/head", aeronet::http::Method::GET,
                       [](const aeronet::HttpRequest& /*unused*/, aeronet::HttpResponseWriter& writer) {
                         writer.statusCode(200);
                         writer.customHeader("Content-Type", "text/plain");
                         writer.write("SHOULD_NOT_APPEAR");  // for HEAD this must be suppressed by writer
                         writer.end();
                       });
  std::jthread th([&] { srv.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string headResp = httpRequest(srv.port(), "HEAD", "/head");
  // Body should be empty; ensure word not present and Content-Length: 0 (or if chunked not used at all)
  auto headerEnd = headResp.find("\r\n\r\n");
  ASSERT_NE(std::string::npos, headerEnd);
  std::string bodyPart = headResp.substr(headerEnd + 4);
  EXPECT_TRUE(bodyPart.empty());
  // Either explicit Content-Length: 0 is present or (future) alternate header; assert current behavior.
  EXPECT_NE(std::string::npos, headResp.find("Content-Length: 0"));
  EXPECT_EQ(std::string::npos, headResp.find("SHOULD_NOT_APPEAR"));
  srv.stop();
}

TEST(HttpServerMixed, MethodNotAllowedWhenOnlyOtherStreamingMethodRegistered) {
  aeronet::HttpServerConfig cfg;
  cfg.enableKeepAlive = false;
  aeronet::HttpServer srv(cfg);
  // Register only GET streaming handler
  srv.router().setPath("/m405", aeronet::http::Method::GET,
                       [](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
                         writer.statusCode(200);
                         writer.write("OKGET");
                         writer.end();
                       });
  std::jthread th([&] { srv.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string postResp = httpRequest(srv.port(), "POST", "/m405", "data");
  // Expect 405 Method Not Allowed
  EXPECT_NE(std::string::npos, postResp.find("405"));
  EXPECT_NE(std::string::npos, postResp.find("Method Not Allowed"));
  // Ensure GET still works and returns streaming body
  std::string getResp2 = httpRequest(srv.port(), "GET", "/m405");
  auto decoded2 = extractBody(getResp2);
  EXPECT_EQ(decoded2, "OKGET");
  srv.stop();
}

TEST(HttpServerMixed, KeepAliveSequentialMixedStreamingAndNormal) {
  aeronet::HttpServerConfig cfg;
  cfg.enableKeepAlive = true;
  cfg.maxRequestsPerConnection = 3;  // allow at least two
  aeronet::HttpServer srv(cfg);
  // Register streaming GET and normal POST on same path
  srv.router().setPath("/ka", aeronet::http::Method::GET,
                       [](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
                         writer.statusCode(200);
                         writer.customHeader("Content-Type", "text/plain");
                         writer.write("A");
                         writer.write("B");
                         writer.end();
                       });
  srv.router().setPath("/ka", aeronet::http::Method::POST, [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse(201).reason("Created").body("NORMAL").contentType("text/plain");
  });
  std::jthread th([&] { srv.run(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  // Build raw requests (each must include Host and Connection: keep-alive)
  std::string r1 = "GET /ka HTTP/1.1\r\nHost: test\r\nConnection: keep-alive\r\n\r\n";  // streaming
  std::string r2 =
      "POST /ka HTTP/1.1\r\nHost: test\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";  // normal, closes

  aeronet::test::ClientConnection cnx(srv.port());

  aeronet::test::sendAll(cnx.fd(), r1 + r2);

  std::string raw = aeronet::test::recvUntilClosed(cnx.fd());

  // Should contain two HTTP/1.1 status lines, first 200 OK, second 201 Created
  auto firstPos = raw.find("200 OK");
  auto secondPos = raw.find("201 Created");
  EXPECT_NE(std::string::npos, firstPos);
  EXPECT_NE(std::string::npos, secondPos);
  // Decode first body (chunked) expecting AB
  auto firstHeaderEnd = raw.find(aeronet::http::DoubleCRLF);
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
