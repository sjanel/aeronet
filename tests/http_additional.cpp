#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <thread>

#include "aeronet/server.hpp"
#include "test_util.hpp"

using namespace std::chrono_literals;

// New tests: pipelining, zero-length Expect (no 100), maxRequestsPerConnection, pipeline error after success, 413 via
// large Content-Length

TEST(HttpPipeline, TwoRequestsBackToBack) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse respObj;
    respObj.body = std::string("E:") + std::string(req.target);
    return respObj;
  });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(60ms);
  int fd = tu_connect(port);
  ASSERT_GE(fd, 0);
  std::string combo =
      "GET /a HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\nContent-Length: "
      "0\r\nConnection: close\r\n\r\n";
  tu_sendAll(fd, combo);
  std::string resp = tu_recvUntilClosed(fd);
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("E:/a"));
  ASSERT_NE(std::string::npos, resp.find("E:/b"));
}

TEST(HttpExpect, ZeroLengthNo100) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest& /*req*/) {
    aeronet::HttpResponse respObj;
    respObj.body = "Z";
    return respObj;
  });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(60ms);
  int fd = tu_connect(port);
  ASSERT_GE(fd, 0);
  std::string headers =
      "POST /z HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";  // zero
                                                                                                                // length
                                                                                                                // -> no
                                                                                                                // 100
                                                                                                                // expected
  tu_sendAll(fd, headers);
  std::string resp = tu_recvUntilClosed(fd);
  server.stop();
  th.join();
  ASSERT_EQ(std::string::npos, resp.find("100 Continue"));
  ASSERT_NE(std::string::npos, resp.find('Z'));
}

TEST(HttpMaxRequests, CloseAfterLimit) {
  aeronet::ServerConfig cfg;
  cfg.withMaxRequestsPerConnection(2);  // after 2 responses close
  aeronet::HttpServer server(cfg);
  uint16_t port = server.port();
  // Use a distinctive body character unlikely to appear in headers (avoid 'M' which can appear in Date: Mon)
  server.setHandler([](const aeronet::HttpRequest& /*req*/) {
    aeronet::HttpResponse respObj;
    respObj.body = "Q";
    return respObj;
  });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(60ms);
  int fd = tu_connect(port);
  ASSERT_GE(fd, 0);
  std::string reqs =
      "GET /1 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nGET /2 HTTP/1.1\r\nHost: x\r\nContent-Length: "
      "0\r\n\r\nGET /3 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
  tu_sendAll(fd, reqs);
  std::string resp = tu_recvUntilClosed(fd);
  server.stop();
  th.join();
  // Count complete HTTP response status lines to ensure only two responses emitted
  int statusCount = 0;
  size_t pos = 0;
  while ((pos = resp.find("HTTP/1.1 200", pos)) != std::string::npos) {
    ++statusCount;
    pos += 11;
  }
  ASSERT_EQ(2, statusCount);
  // Ensure exactly two body markers 'Q' are present
  int bodyCount = 0;
  pos = 0;
  while ((pos = resp.find('Q', pos)) != std::string::npos) {
    ++bodyCount;
    ++pos;
  }
  ASSERT_EQ(2, bodyCount);
}

TEST(HttpPipeline, SecondMalformedAfterSuccess) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest& /*req*/) {
    aeronet::HttpResponse respObj;
    respObj.body = "OK";
    return respObj;
  });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(60ms);
  int fd = tu_connect(port);
  ASSERT_GE(fd, 0);
  std::string piped = "GET /good HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nBADSECOND\r\n\r\n";
  tu_sendAll(fd, piped);
  std::string resp = tu_recvUntilClosed(fd);
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("OK"));
  ASSERT_NE(std::string::npos, resp.find("400"));
}

TEST(HttpContentLength, ExplicitTooLarge413) {
  aeronet::ServerConfig cfg;
  cfg.withMaxBodyBytes(10);
  aeronet::HttpServer server(cfg);
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest& /*req*/) {
    aeronet::HttpResponse respObj;
    respObj.body = "R";
    return respObj;
  });
  std::thread th([&] { server.runUntil([] { return false; }, 30ms); });
  std::this_thread::sleep_for(60ms);
  int fd = tu_connect(port);
  ASSERT_GE(fd, 0);
  // Declare length 20 (exceeds limit 10) but send no body bytes
  std::string req = "POST /big HTTP/1.1\r\nHost: x\r\nContent-Length: 20\r\nConnection: close\r\n\r\n";
  tu_sendAll(fd, req);
  std::string resp = tu_recvUntilClosed(fd);
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("413"));
}
