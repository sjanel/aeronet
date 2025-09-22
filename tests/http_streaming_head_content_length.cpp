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
std::string raw(uint16_t port, const std::string& verb) {
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
  std::string req = verb + " /len HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  char buf[4096];
  std::string out;
  ssize_t bytesRead;
  while ((bytesRead = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
    out.append(buf, buf + bytesRead);
  }
  ::close(fd);
  return out;
}
}  // namespace

TEST(HttpStreamingHeadContentLength, HeadSuppressesBodyKeepsCL) {
  aeronet::ServerConfig cfg;
  cfg.withMaxRequestsPerConnection(2);
  aeronet::HttpServer server(cfg);
  server.setStreamingHandler([]([[maybe_unused]] const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& writer) {
    writer.setStatus(200, "OK");
    // We set Content-Length even though we write body pieces; for HEAD the body must be suppressed but CL retained.
    const std::string body = "abcdef";  // length 6
    writer.setContentLength(body.size());
    writer.write(body.substr(0, 3));
    writer.write(body.substr(3));
    writer.end();
  });
  auto port = server.port();
  std::jthread th([&] { server.runUntil([] { return false; }, 50ms); });
  std::this_thread::sleep_for(100ms);
  std::string headResp = raw(port, "HEAD");
  std::string getResp = raw(port, "GET");
  server.stop();

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
