#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstdint>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"

using namespace std::chrono_literals;

namespace {
std::string doRequest(uint16_t port, const std::string& verb, const std::string& target) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return {};
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return {};
  }
  std::string req = verb + " " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  char buf[8192];
  std::string out;
  ssize_t bytesRead;
  while ((bytesRead = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
    out.append(buf, buf + bytesRead);
  }
  ::close(fd);
  return out;
}
}  // namespace

// Coverage goals:
// 1. setHeader emits custom headers.
// 2. Multiple calls with unique names all appear.
// 3. Overriding Content-Type via setHeader before any body suppresses default text/plain.
// 4. Calling setHeader after headers were implicitly sent (by first write) has no effect.
// 5. HEAD request: headers still emitted correctly without body/chunk framing; Content-Length auto added when absent.

TEST(HttpStreamingSetHeader, MultipleCustomHeadersAndOverrideContentType) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  auto port = server.port();
  server.setStreamingHandler([](const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& writer) {
    bool isHead = req.method == "HEAD";
    writer.setStatus(200, "OK");
    writer.setHeader("X-Custom-A", "alpha");
    writer.setHeader("X-Custom-B", "beta");
    writer.setHeader("Content-Type", "application/json");  // override default
    // First write sends headers implicitly.
    writer.write("{\"k\":1}");
    // These should be ignored because headers already sent.
    writer.setHeader("X-Ignored", "zzz");
    writer.setHeader("Content-Type", "text/plain");
    writer.end();
    if (isHead) {
      // Nothing extra; body suppressed automatically.
    }
  });
  std::jthread th([&] { server.run(40ms); });
  std::this_thread::sleep_for(80ms);
  std::string getResp = doRequest(port, "GET", "/hdr");
  std::string headResp = doRequest(port, "HEAD", "/hdr");
  server.stop();
  th.join();
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
