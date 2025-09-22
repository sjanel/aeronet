#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"

using namespace std::chrono_literals;

namespace {
std::string sendAndRecv(int fd, const std::string& data) {
  if (!data.empty()) {
    ssize_t sent = ::send(fd, data.data(), data.size(), 0);
    if (std::cmp_not_equal(sent, data.size())) {
      return {};
    }
  }
  std::string out;
  char buf[4096];
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

TEST(HttpChunked, DecodeBasic) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  auto port = server.port();
  server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    // echo body size & content (limited) to verify decoding
    resp.body = std::string("LEN=") + std::to_string(req.body.size()) + ":" + std::string(req.body);
    return resp;
  });
  std::jthread th([&] { server.run(20ms); });
  std::this_thread::sleep_for(80ms);

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(0, ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  std::string req =
      "POST /c HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
  std::string resp = sendAndRecv(fd, req);
  ::close(fd);
  server.stop();
  ASSERT_NE(std::string::npos, resp.find("LEN=9:Wikipedia"));
}

TEST(HttpChunked, RejectTooLarge) {
  aeronet::ServerConfig cfg;
  cfg.withMaxBodyBytes(4);  // very small limit
  aeronet::HttpServer server(cfg);
  auto port = server.port();
  server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body = std::string(req.body);
    return resp;
  });
  std::jthread th([&] { server.runUntil([] { return false; }, 20ms); });
  std::this_thread::sleep_for(80ms);
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(0, ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  // Single 5-byte chunk exceeds limit 4
  std::string req =
      "POST /big HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nabcde\r\n0\r\n\r\n";
  std::string resp = sendAndRecv(fd, req);
  ::close(fd);
  server.stop();
  ASSERT_NE(std::string::npos, resp.find("413"));
}

TEST(HttpHead, NoBodyReturned) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  auto port = server.port();
  server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body = std::string("DATA-") + std::string(req.target);
    return resp;
  });
  std::jthread th([&] { server.runUntil([] { return false; }, 20ms); });
  std::this_thread::sleep_for(80ms);
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(0, ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  std::string req = "HEAD /head HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  std::string resp = sendAndRecv(fd, req);
  ::close(fd);
  server.stop();
  // Should have Content-Length header referencing length of would-be body (which is 10: DATA-/head)
  ASSERT_NE(std::string::npos, resp.find("Content-Length: 10"));
  // And not actually contain DATA-/head bytes after header terminator
  auto hdrEnd = resp.find("\r\n\r\n");
  ASSERT_NE(std::string::npos, hdrEnd);
  std::string after = resp.substr(hdrEnd + 4);
  ASSERT_TRUE(after.empty());
}

TEST(HttpExpect, ContinueFlow) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  auto port = server.port();
  server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse respObj;
    respObj.body = std::string(req.body);
    return respObj;
  });
  std::jthread th([&] { server.runUntil([] { return false; }, 20ms); });
  std::this_thread::sleep_for(60ms);
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(0, ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  std::string headers =
      "POST /e HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  ssize_t hs = ::send(fd, headers.data(), headers.size(), 0);
  ASSERT_EQ(hs, static_cast<ssize_t>(headers.size()));
  // read interim 100
  char buf[128];
  ssize_t firstRead = ::recv(fd, buf, sizeof(buf), 0);
  std::string interim(buf, buf + (firstRead > 0 ? firstRead : 0));
  ASSERT_NE(std::string::npos, interim.find("100 Continue"));
  std::string body = "hello";
  ssize_t bs = ::send(fd, body.data(), body.size(), 0);
  ASSERT_EQ(bs, static_cast<ssize_t>(body.size()));
  std::string full = interim + sendAndRecv(fd, "");
  ::close(fd);
  server.stop();
  ASSERT_NE(std::string::npos, full.find("hello"));
}
