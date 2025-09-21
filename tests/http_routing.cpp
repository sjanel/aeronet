#include <gtest/gtest.h>

#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "exception.hpp"
#include "test_util.hpp"

using namespace aeronet;

TEST(HttpRouting, BasicPathDispatch) {
  ServerConfig cfg;
  cfg.withPort(0).withMaxRequestsPerConnection(10);
  HttpServer server(cfg);
  http::MethodSet helloMethods{http::Method::GET};
  server.addPathHandler("/hello", helloMethods, [](const HttpRequest&) {
    HttpResponse resp;
    resp.statusCode = 200;
    resp.reason = "OK";
    resp.body = "world";
    resp.contentType = "text/plain";
    return resp;
  });
  http::MethodSet multiMethods{http::Method::GET, http::Method::POST};
  server.addPathHandler("/multi", multiMethods, [](const HttpRequest& req) {
    HttpResponse resp;
    resp.statusCode = 200;
    resp.reason = "OK";
    resp.body = string(req.method) + "!";
    resp.contentType = "text/plain";
    return resp;
  });

  std::atomic<bool> done{false};
  std::thread th([&]() { server.runUntil([&]() { return done.load(); }, std::chrono::milliseconds{50}); });
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Helper lambda to perform a raw HTTP request and return response string
  auto doReq = [&](const std::string& raw) -> std::string {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
      return std::string{};
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(server.port());
    bool connected = false;
    for (int attempt = 0; attempt < 50; ++attempt) {
      if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        connected = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!connected) {
      ::close(sock);
      return std::string{};
    }
    if (::send(sock, raw.data(), raw.size(), 0) != static_cast<ssize_t>(raw.size())) {
      ::close(sock);
      return std::string{};
    }
    char buf[1024];
    ssize_t recvCount = ::recv(sock, buf, sizeof(buf), 0);
    std::string resp;
    if (recvCount > 0) {
      resp.assign(buf, buf + recvCount);
    }
    ::close(sock);
    return resp;
  };

  std::string resp1 = doReq("GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_NE(resp1.find("200 OK"), std::string::npos);
  EXPECT_NE(resp1.find("world"), std::string::npos);

  std::string resp2 = doReq("POST /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
  EXPECT_NE(resp2.find("405 Method Not Allowed"), std::string::npos);

  std::string resp3 = doReq("GET /missing HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_NE(resp3.find("404 Not Found"), std::string::npos);

  std::string resp4 = doReq("POST /multi HTTP/1.1\r\nHost: x\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
  EXPECT_NE(resp4.find("200 OK"), std::string::npos);
  EXPECT_NE(resp4.find("POST!"), std::string::npos);

  done.store(true);
  th.join();
}

TEST(HttpRouting, ExclusivityWithGlobalHandler) {
  ServerConfig cfg;
  cfg.withPort(0);
  HttpServer server(cfg);
  server.setHandler([](const HttpRequest&) {
    HttpResponse resp;
    resp.statusCode = 200;
    resp.reason = "OK";
    return resp;
  });
  // Adding path handler after global handler should throw
  http::MethodSet xMethods{http::Method::GET};
  EXPECT_THROW(server.addPathHandler("/x", xMethods, [](const HttpRequest&) { return HttpResponse{}; }),
               aeronet::exception);
}
