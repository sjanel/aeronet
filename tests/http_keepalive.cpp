#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "socket.hpp"
#include "test_server_fixture.hpp"

using namespace std::chrono_literals;

namespace {
std::string sendRaw(int fd, const std::string& data) {
  auto sent = ::send(fd, data.data(), data.size(), 0);
  if (std::cmp_not_equal(sent, data.size())) {
    return {};
  }
  char buf[4096];
  std::string out;
  // simple read with small timeout loop
  for (int i = 0; i < 50; ++i) {
    auto bytes = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
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
  TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body = std::string("ECHO") + std::string(req.target);
    return resp;
  });

  aeronet::Socket fdSock(aeronet::Socket::Type::STREAM);
  int fd = fdSock.fd();
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

  fdSock.close();
  ts.stop();
}

TEST(HttpLimits, RejectHugeHeaders) {
  aeronet::HttpServerConfig cfg;
  cfg.maxHeaderBytes = 128;
  cfg.enableKeepAlive = false;
  cfg.port = 0;
  TestServer ts(cfg);
  auto port = ts.port();
  ts.server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body = "OK";
    return resp;
  });

  aeronet::Socket fdSock2(aeronet::Socket::Type::STREAM);
  int fd = fdSock2.fd();
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(0, ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));

  std::string bigHeader(200, 'a');
  std::string req = "GET /h HTTP/1.1\r\nHost: x\r\nX-Big: " + bigHeader + "\r\n\r\n";
  std::string resp = sendRaw(fd, req);
  EXPECT_NE(std::string::npos, resp.find("431"));
  fdSock2.close();
  ts.stop();
}
