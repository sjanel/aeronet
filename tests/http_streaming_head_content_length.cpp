#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <cstdint>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"

using namespace std::chrono_literals;

namespace {
void raw(uint16_t port, const std::string& verb, std::string& out) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0) << "socket failed";
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  int cRet = connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
  ASSERT_EQ(cRet, 0) << "connect failed";
  std::string req = verb + " /len HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  ssize_t sent = ::send(fd, req.data(), req.size(), 0);
  ASSERT_EQ(sent, static_cast<ssize_t>(req.size())) << "send partial";
  char buf[4096];
  out.clear();
  while (true) {
    ssize_t bytesRead = ::recv(fd, buf, sizeof(buf), 0);
    if (bytesRead <= 0) {
      break;
    }
    out.append(buf, buf + bytesRead);
  }
  ::close(fd);
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
  std::string headResp;
  std::string getResp;
  raw(port, "HEAD", headResp);
  raw(port, "GET", getResp);
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
