#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "socket.hpp"
#include "test_server_fixture.hpp"
#include "test_util.hpp"

using namespace std::chrono_literals;

namespace {
std::string sendAndRecv(int fd, const std::string& data) {
  if (!data.empty()) {
    auto sent = ::send(fd, data.data(), data.size(), 0);
    if (std::cmp_not_equal(sent, data.size())) {
      return {};
    }
  }
  std::string out;
  char buf[4096];
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

TEST(HttpChunked, DecodeBasic) {
  TestServer ts(aeronet::ServerConfig{});
  auto port = ts.port();
  ts.server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    // echo body size & content (limited) to verify decoding
    resp.body = std::string("LEN=") + std::to_string(req.body.size()) + ":" + std::string(req.body);
    return resp;
  });

  ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /c HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
  std::string resp = sendAndRecv(fd, req);
  // automatic close via Socket dtor
  ts.stop();
  ASSERT_NE(std::string::npos, resp.find("LEN=9:Wikipedia"));
}

TEST(HttpChunked, RejectTooLarge) {
  aeronet::ServerConfig cfg;
  cfg.withMaxBodyBytes(4);  // very small limit
  TestServer ts(cfg);
  auto port = ts.port();
  ts.server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body = std::string(req.body);
    return resp;
  });
  aeronet::Socket sock(aeronet::Socket::Type::STREAM);
  int fd = sock.fd();
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(0, ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  // Single 5-byte chunk exceeds limit 4
  std::string req =
      "POST /big HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nabcde\r\n0\r\n\r\n";
  std::string resp = sendAndRecv(fd, req);
  // automatic close via Socket dtor
  ts.stop();
  ASSERT_NE(std::string::npos, resp.find("413"));
}

TEST(HttpHead, NoBodyReturned) {
  TestServer ts(aeronet::ServerConfig{});
  auto port = ts.port();
  ts.server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body = std::string("DATA-") + std::string(req.target);
    return resp;
  });
  aeronet::Socket sock(aeronet::Socket::Type::STREAM);
  int fd = sock.fd();
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(0, ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  std::string req = "HEAD /head HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  std::string resp = sendAndRecv(fd, req);
  // automatic close via Socket dtor
  ts.stop();
  // Should have Content-Length header referencing length of would-be body (which is 10: DATA-/head)
  ASSERT_NE(std::string::npos, resp.find("Content-Length: 10"));
  // And not actually contain DATA-/head bytes after header terminator
  auto hdrEnd = resp.find("\r\n\r\n");
  ASSERT_NE(std::string::npos, hdrEnd);
  std::string after = resp.substr(hdrEnd + 4);
  ASSERT_TRUE(after.empty());
}

TEST(HttpExpect, ContinueFlow) {
  TestServer ts(aeronet::ServerConfig{});
  auto port = ts.port();
  ts.server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse respObj;
    respObj.body = std::string(req.body);
    return respObj;
  });
  aeronet::Socket sock(aeronet::Socket::Type::STREAM);
  int fd = sock.fd();
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(0, ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)));
  std::string headers =
      "POST /e HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  auto hs = ::send(fd, headers.data(), headers.size(), 0);
  ASSERT_EQ(hs, static_cast<decltype(hs)>(headers.size()));
  // read interim 100
  char buf[128];
  auto firstRead = ::recv(fd, buf, sizeof(buf), 0);
  std::string interim(buf, buf + (firstRead > 0 ? firstRead : 0));
  ASSERT_NE(std::string::npos, interim.find("100 Continue"));
  std::string body = "hello";
  auto bs = ::send(fd, body.data(), body.size(), 0);
  ASSERT_EQ(bs, static_cast<decltype(hs)>(body.size()));
  std::string full = interim + sendAndRecv(fd, "");
  // automatic close via Socket dtor
  ts.stop();
  ASSERT_NE(std::string::npos, full.find("hello"));
}
