#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <thread>

#include "aeronet/server.hpp"

using namespace std::chrono_literals;

namespace {
std::string sendRaw(int fd, const std::string& data) {
  ::send(fd, data.data(), data.size(), 0);
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
  uint16_t port = 18321;
  aeronet::HttpServer server(aeronet::ServerConfig{}.withPort(port));
  server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body = std::string("ECHO") + std::string(req.target);
    return resp;
  });
  std::thread th([&] { server.runUntil([] { return false; }, 50ms); });
  std::this_thread::sleep_for(100ms);

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(18321);
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
  cfg.port = 18322;
  aeronet::HttpServer server(cfg);
  server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body = "OK";
    return resp;
  });
  std::thread th([&] { server.runUntil([] { return false; }, 50ms); });
  std::this_thread::sleep_for(100ms);

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(18322);
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
