#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <thread>

#include "aeronet/server.hpp"
#include "test_util.hpp"

using namespace std::chrono_literals;

namespace {
std::string collectSimple(uint16_t port, const std::string& req) {
  int fd = tu_connect(port);
  EXPECT_GE(fd, 0);
  tu_sendAll(fd, req);
  std::string resp = tu_recvUntilClosed(fd);
  return resp;
}
}  // namespace

TEST(Http10, BasicVersionEcho) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    respObj.body = "A";
    return respObj;
  });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(60ms);
  std::string req = "GET /x HTTP/1.0\r\nHost: h\r\n\r\n";
  std::string resp = collectSimple(port, req);
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("HTTP/1.0 200"));
}

TEST(Http10, No100ContinueEvenIfHeaderPresent) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    respObj.body = "B";
    return respObj;
  });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(60ms);
  std::string req =
      "POST /p HTTP/1.0\r\nHost: h\r\nContent-Length: 0\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";  // Expect
                                                                                                                // should
                                                                                                                // be
                                                                                                                // ignored
                                                                                                                // for 1.0
  std::string resp = collectSimple(server.port(), req);
  server.stop();
  th.join();
  ASSERT_EQ(std::string::npos, resp.find("100 Continue"));
  ASSERT_NE(std::string::npos, resp.find("HTTP/1.0 200"));
}

TEST(Http10, RejectTransferEncoding) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    respObj.body = "C";
    return respObj;
  });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(60ms);
  std::string req = "GET /te HTTP/1.0\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
  std::string resp = collectSimple(port, req);
  server.stop();
  th.join();
  // Should return 400 per implementation decision
  ASSERT_NE(std::string::npos, resp.find("400"));
}

TEST(Http10, KeepAliveOptInStillWorks) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse respObj;
    respObj.body = "D";
    return respObj;
  });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(60ms);
  int fd = tu_connect(port);
  ASSERT_GE(fd, 0);
  std::string req1 = "GET /k1 HTTP/1.0\r\nHost: h\r\nConnection: keep-alive\r\n\r\n";
  tu_sendAll(fd, req1);
  std::string first = tu_recvWithTimeout(fd, 300ms);
  ASSERT_NE(std::string::npos, first.find("HTTP/1.0 200"));
  ASSERT_NE(std::string::npos, first.find("Connection: keep-alive"));
  std::string req2 = "GET /k2 HTTP/1.0\r\nHost: h\r\nConnection: keep-alive\r\n\r\n";
  tu_sendAll(fd, req2);
  std::string second = tu_recvWithTimeout(fd, 300ms);
  ASSERT_NE(std::string::npos, second.find("HTTP/1.0 200"));
  ::close(fd);
  server.stop();
  th.join();
}
