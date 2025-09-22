#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"

using namespace std::chrono_literals;

namespace {
std::string sendRaw(int fd, const std::string& data) {
  ssize_t sent = ::send(fd, data.data(), data.size(), 0);
  if (sent != static_cast<ssize_t>(data.size())) {
    return {};
  }
  char buf[4096];
  std::string out;
  // simple read with small timeout loop
  for (int i = 0; i < 50; ++i) {
    ssize_t bytes = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (bytes > 0) {
      out.append(buf, buf + bytes);
    } else if (bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      std::this_thread::sleep_for(10ms);
      continue;
    } else {
      break;
    }
  }
  return out;
}
}  // namespace

TEST(HttpKeepAlive, MultipleSequentialRequests) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  auto port = server.port();
  server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body = std::string("ECHO") + std::string(req.target);
    return resp;
  });
  std::jthread th([&] { server.runUntil([] { return false; }, 50ms); });
  std::this_thread::sleep_for(100ms);

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(0, ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));

  std::string req1 = "GET /one HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\nContent-Length: 0\r\n\r\n";
  std::string resp1 = sendRaw(fd, req1);
  EXPECT_NE(std::string::npos, resp1.find("ECHO/one"));
  EXPECT_NE(std::string::npos, resp1.find("Connection: keep-alive"));

  std::string req2 = "GET /two HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";  // implicit keep-alive
  std::string resp2 = sendRaw(fd, req2);
  EXPECT_NE(std::string::npos, resp2.find("ECHO/two"));

  ::close(fd);
  server.stop();
  th.join();
}

TEST(HttpLimits, RejectHugeHeaders) {
  aeronet::ServerConfig cfg;
  cfg.maxHeaderBytes = 128;
  cfg.enableKeepAlive = false;
  cfg.port = 0;
  aeronet::HttpServer server(cfg);
  auto port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body = "OK";
    return resp;
  });
  std::jthread th([&] { server.runUntil([] { return false; }, 50ms); });
  std::this_thread::sleep_for(100ms);

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(0, ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));

  std::string bigHeader(200, 'a');
  std::string req = "GET /h HTTP/1.1\r\nHost: x\r\nX-Big: " + bigHeader + "\r\n\r\n";
  std::string resp = sendRaw(fd, req);
  EXPECT_NE(std::string::npos, resp.find("431"));
  ::close(fd);
  server.stop();
  th.join();
}
