#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "exception.hpp"
#include "http-method-set.hpp"
#include "http-method.hpp"

namespace {
void httpRequest(uint16_t port, const std::string& method, const std::string& path, std::string& out,
                 const std::string& body = {}) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0) << "socket failed";
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  int cRet = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  ASSERT_EQ(cRet, 0) << "connect failed";
  std::string req = method + " " + path + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n";
  if (!body.empty()) {
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  }
  req += "\r\n";
  req += body;
  ssize_t sent = ::send(fd, req.data(), req.size(), 0);
  ASSERT_EQ(sent, static_cast<ssize_t>(req.size())) << "send partial";
  out.clear();
  char buf[4096];
  while (true) {
    ssize_t bytesRead = ::recv(fd, buf, sizeof(buf), 0);
    if (bytesRead <= 0) {
      break;
    }
    out.append(buf, static_cast<std::size_t>(bytesRead));
  }
  ::close(fd);
}

// Very small chunked decoder for tests (single pass, no trailers). Expects full HTTP response.
std::string extractBody(const std::string& resp) {
  auto headerEnd = resp.find("\r\n\r\n");
  if (headerEnd == std::string::npos) {
    return {};
  }
  std::string body = resp.substr(headerEnd + 4);
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
  aeronet::ServerConfig cfg;
  cfg.port = 0;
  cfg.enableKeepAlive = false;
  aeronet::HttpServer srv(cfg);
  // path /mix : GET streaming, POST normal
  aeronet::http::MethodSet getSet;
  getSet.insert(aeronet::http::Method::GET);
  aeronet::http::MethodSet postSet;
  postSet.insert(aeronet::http::Method::POST);
  srv.addPathStreamingHandler("/mix", getSet,
                              [](const aeronet::HttpRequest& /*unused*/, aeronet::HttpResponseWriter& writer) {
                                writer.setStatus(200, "OK");
                                writer.setHeader("Content-Type", "text/plain");
                                writer.write("S");
                                writer.write("TREAM");
                                writer.end();
                              });
  srv.addPathHandler("/mix", postSet, [](const aeronet::HttpRequest& /*unused*/) {
    aeronet::HttpResponse resp;
    resp.statusCode = 201;
    resp.reason = "Created";
    resp.contentType = "text/plain";
    resp.body = "NORMAL";
    return resp;
  });
  std::jthread th([&] { srv.runUntil([] { return false; }, std::chrono::milliseconds(5)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string getResp;
  httpRequest(srv.port(), "GET", "/mix", getResp);
  auto decoded = extractBody(getResp);
  EXPECT_EQ(decoded, "STREAM");
  std::string postResp;
  httpRequest(srv.port(), "POST", "/mix", postResp, "x");
  EXPECT_NE(std::string::npos, postResp.find("NORMAL"));
  srv.stop();
}

TEST(HttpServerMixed, ConflictRegistrationNormalThenStreaming) {
  aeronet::ServerConfig cfg;
  cfg.port = 0;
  aeronet::HttpServer srv(cfg);
  srv.addPathHandler("/c", aeronet::http::Method::GET,
                     [](const aeronet::HttpRequest&) { return aeronet::HttpResponse{200, "OK", "text/plain", "X"}; });
  EXPECT_THROW(srv.addPathStreamingHandler("/c", aeronet::http::Method::GET,
                                           [](const aeronet::HttpRequest&, aeronet::HttpResponseWriter&) {}),
               aeronet::exception);
}

TEST(HttpServerMixed, ConflictRegistrationStreamingThenNormal) {
  aeronet::ServerConfig cfg;
  cfg.port = 0;
  aeronet::HttpServer srv(cfg);
  srv.addPathStreamingHandler("/c2", aeronet::http::Method::GET,
                              [](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
                                writer.setStatus(200, "OK");
                                writer.end();
                              });
  EXPECT_THROW(srv.addPathHandler(
                   "/c2", aeronet::http::Method::GET,
                   [](const aeronet::HttpRequest&) { return aeronet::HttpResponse{200, "OK", "text/plain", "Y"}; }),
               aeronet::exception);
}

TEST(HttpServerMixed, GlobalFallbackPrecedence) {
  aeronet::ServerConfig cfg;
  cfg.port = 0;
  cfg.enableKeepAlive = false;
  aeronet::HttpServer srv(cfg);
  srv.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.statusCode = 200;
    resp.reason = "OK";
    resp.contentType = "text/plain";
    resp.body = "GLOBAL";
    return resp;
  });
  srv.setStreamingHandler([](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
    writer.setStatus(200, "OK");
    writer.setHeader("Content-Type", "text/plain");
    writer.write("STREAMFALLBACK");
    writer.end();
  });
  // path-specific streaming overrides both
  srv.addPathStreamingHandler("/s", aeronet::http::Method::GET,
                              [](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
                                writer.setStatus(200, "OK");
                                writer.write("PS");
                                writer.end();
                              });
  // path-specific normal overrides global fallbacks
  srv.addPathHandler("/n", aeronet::http::Method::GET,
                     [](const aeronet::HttpRequest&) { return aeronet::HttpResponse{200, "OK", "text/plain", "PN"}; });
  std::jthread th([&] { srv.runUntil([] { return false; }, std::chrono::milliseconds(5)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string pathStreamResp;
  httpRequest(srv.port(), "GET", "/s", pathStreamResp);
  EXPECT_NE(std::string::npos, pathStreamResp.find("PS"));
  std::string pathNormalResp;
  httpRequest(srv.port(), "GET", "/n", pathNormalResp);
  EXPECT_NE(std::string::npos, pathNormalResp.find("PN"));
  std::string fallback;
  httpRequest(srv.port(), "GET", "/other", fallback);
  // Should use global streaming first (higher precedence than global normal)
  EXPECT_NE(std::string::npos, fallback.find("STREAMFALLBACK"));
  srv.stop();
}

TEST(HttpServerMixed, GlobalNormalOnlyWhenNoStreaming) {
  aeronet::ServerConfig cfg;
  cfg.port = 0;
  cfg.enableKeepAlive = false;
  aeronet::HttpServer srv(cfg);
  srv.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{200, "OK", "text/plain", "GN"}; });
  std::jthread th([&] { srv.runUntil([] { return false; }, std::chrono::milliseconds(5)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  std::string result;
  httpRequest(srv.port(), "GET", "/x", result);
  EXPECT_NE(std::string::npos, result.find("GN"));
  srv.stop();
}

TEST(HttpServerMixed, HeadRequestOnStreamingPathSuppressesBody) {
  aeronet::ServerConfig cfg;
  cfg.port = 0;
  cfg.enableKeepAlive = false;
  aeronet::HttpServer srv(cfg);
  aeronet::http::MethodSet getSet;
  getSet.insert(aeronet::http::Method::GET);  // rely on implicit HEAD->GET fallback
  // Register streaming handler for GET; it will attempt to write a body.
  srv.addPathStreamingHandler("/head", getSet,
                              [](const aeronet::HttpRequest& /*unused*/, aeronet::HttpResponseWriter& writer) {
                                writer.setStatus(200, "OK");
                                writer.setHeader("Content-Type", "text/plain");
                                writer.write("SHOULD_NOT_APPEAR");  // for HEAD this must be suppressed by writer
                                writer.end();
                              });
  std::jthread th([&] { srv.runUntil([] { return false; }, std::chrono::milliseconds(5)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string headResp;
  httpRequest(srv.port(), "HEAD", "/head", headResp);
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
  aeronet::ServerConfig cfg;
  cfg.port = 0;
  cfg.enableKeepAlive = false;
  aeronet::HttpServer srv(cfg);
  // Register only GET streaming handler
  srv.addPathStreamingHandler("/m405", aeronet::http::Method::GET,
                              [](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
                                writer.setStatus(200, "OK");
                                writer.write("OKGET");
                                writer.end();
                              });
  std::jthread th([&] { srv.runUntil([] { return false; }, std::chrono::milliseconds(5)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string postResp;
  httpRequest(srv.port(), "POST", "/m405", postResp, "data");
  // Expect 405 Method Not Allowed
  EXPECT_NE(std::string::npos, postResp.find("405"));
  EXPECT_NE(std::string::npos, postResp.find("Method Not Allowed"));
  // Ensure GET still works and returns streaming body
  std::string getResp2;
  httpRequest(srv.port(), "GET", "/m405", getResp2);
  auto decoded2 = extractBody(getResp2);
  EXPECT_EQ(decoded2, "OKGET");
  srv.stop();
}

namespace {
// Helper that performs two sequential HTTP/1.1 requests over a single keep-alive connection and returns the raw
// concatenated responses. Each request must include Connection: keep-alive and server must support it.
void twoRequestsKeepAlive(uint16_t port, const std::string& r1, const std::string& r2, std::string& out) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0) << "socket failed";
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  int cRet = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  ASSERT_EQ(cRet, 0) << "connect failed";
  std::string reqs = r1 + r2;
  ssize_t sentAll = ::send(fd, reqs.data(), reqs.size(), 0);
  ASSERT_EQ(sentAll, static_cast<ssize_t>(reqs.size())) << "send partial";
  out.clear();
  char buf[8192];
  while (true) {
    ssize_t bytesRead = ::recv(fd, buf, sizeof(buf), 0);
    if (bytesRead <= 0) {
      break;
    }
    out.append(buf, static_cast<std::size_t>(bytesRead));
  }
  ::close(fd);
}
}  // namespace

TEST(HttpServerMixed, KeepAliveSequentialMixedStreamingAndNormal) {
  aeronet::ServerConfig cfg;
  cfg.port = 0;
  cfg.enableKeepAlive = true;
  cfg.maxRequestsPerConnection = 3;  // allow at least two
  aeronet::HttpServer srv(cfg);
  // Register streaming GET and normal POST on same path
  aeronet::http::MethodSet getSet;
  getSet.insert(aeronet::http::Method::GET);
  aeronet::http::MethodSet postSet;
  postSet.insert(aeronet::http::Method::POST);
  srv.addPathStreamingHandler("/ka", getSet, [](const aeronet::HttpRequest&, aeronet::HttpResponseWriter& writer) {
    writer.setStatus(200, "OK");
    writer.setHeader("Content-Type", "text/plain");
    writer.write("A");
    writer.write("B");
    writer.end();
  });
  srv.addPathHandler("/ka", postSet, [](const aeronet::HttpRequest&) {
    return aeronet::HttpResponse{201, "Created", "text/plain", "NORMAL"};
  });
  std::jthread th([&] { srv.runUntil([] { return false; }, std::chrono::milliseconds(5)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  // Build raw requests (each must include Host and Connection: keep-alive)
  std::string r1 = "GET /ka HTTP/1.1\r\nHost: test\r\nConnection: keep-alive\r\n\r\n";  // streaming
  std::string r2 =
      "POST /ka HTTP/1.1\r\nHost: test\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";  // normal, closes
  std::string raw;
  twoRequestsKeepAlive(srv.port(), r1, r2, raw);
  // Should contain two HTTP/1.1 status lines, first 200 OK, second 201 Created
  auto firstPos = raw.find("200 OK");
  auto secondPos = raw.find("201 Created");
  EXPECT_NE(std::string::npos, firstPos);
  EXPECT_NE(std::string::npos, secondPos);
  // Decode first body (chunked) expecting AB
  auto firstHeaderEnd = raw.find("\r\n\r\n");
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
