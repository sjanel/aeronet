#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <thread>

#include "aeronet/server.hpp"
#include "test_util.hpp"

using namespace std::chrono_literals;

namespace {
std::string sendAndCollect(uint16_t port, const std::string& raw) {
  int fd = tu_connect(port);
  if (fd < 0) {
    return {};
  }
  tu_sendAll(fd, raw);
  std::string out = tu_recvUntilClosed(fd);
  ::close(fd);
  return out;
}
}  // namespace

TEST(HttpErrors, BadRequestMalformedRequestLine) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(80ms);
  std::string resp = sendAndCollect(port, "GETONLY\r\n\r\n");
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("400"));
}

TEST(HttpErrors, VersionNotSupported505) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(80ms);
  std::string req = "GET /test HTTP/2.0\r\nHost: x\r\n\r\n";  // HTTP/2.0 not supported
  std::string resp = sendAndCollect(port, req);
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("505"));
}

TEST(HttpErrors, UnsupportedTransferEncoding501) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(80ms);
  std::string req =
      "POST /u HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\nConnection: close\r\n\r\n";  // unsupported TE
  std::string resp = sendAndCollect(port, req);
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("501"));
}

TEST(HttpErrors, ContentLengthAndTEConflict400) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(80ms);
  std::string req =
      "POST /c HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\nConnection: "
      "close\r\n\r\nhello";
  std::string resp = sendAndCollect(port, req);
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("400"));
}

TEST(HttpKeepAlive10, DefaultCloseWithoutHeader) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse response;
    response.body = "ok";
    return response;
  });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(80ms);
  // HTTP/1.0 without Connection: keep-alive should close
  int fd = tu_connect(port);
  ASSERT_GE(fd, 0);
  std::string req = "GET /h HTTP/1.0\r\nHost: x\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  char buf[512];
  std::string resp;
  ssize_t bytesRead;
  while ((bytesRead = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
    resp.append(buf, buf + bytesRead);
  }
  ASSERT_NE(std::string::npos, resp.find("Connection: close"));
  // Second request should not yield another response (connection closed). We attempt to read after sending.
  std::string req2 = "GET /h2 HTTP/1.0\r\nHost: x\r\n\r\n";
  ::send(fd, req2.data(), req2.size(), 0);
  char buf2[256];
  ssize_t n2 = ::recv(fd, buf2, sizeof(buf2), 0);
  EXPECT_LE(n2, 0);
  ::close(fd);
  server.stop();
  th.join();
}

TEST(HttpKeepAlive10, OptInWithHeader) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse response;
    response.body = "ok";
    return response;
  });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(80ms);
  int fd = tu_connect(port);
  ASSERT_GE(fd, 0);
  std::string req = "GET /h HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  std::string first;
  char buf[512];
  for (int i = 0; i < 50; ++i) {
    ssize_t received = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (received > 0) {
      first.append(buf, buf + received);
    } else if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      std::this_thread::sleep_for(5ms);
      continue;
    } else {
      break;
    }
    if (first.find("\r\n\r\n") != std::string::npos) {
      break;  // got headers
    }
  }
  ASSERT_NE(std::string::npos, first.find("Connection: keep-alive"));
  std::string req2 = "GET /h2 HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  ::send(fd, req2.data(), req2.size(), 0);
  std::string second;
  for (int i = 0; i < 50; ++i) {
    ssize_t received = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (received > 0) {
      second.append(buf, buf + received);
    } else if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      std::this_thread::sleep_for(5ms);
      continue;
    } else {
      break;
    }
  }
  ASSERT_NE(std::string::npos, second.find("Connection: keep-alive"));
  ::close(fd);
  server.stop();
  th.join();
}
