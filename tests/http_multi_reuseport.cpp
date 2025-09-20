#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <future>
#include <thread>

#include "aeronet/server.hpp"

// This test only validates that two servers can bind the same port with SO_REUSEPORT enabled
// and accept at least one connection each. It does not attempt to assert load distribution.

namespace {
std::string simpleGet(const char* host, uint16_t port, const char* path) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return {};
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  ::inet_pton(AF_INET, host, &addr.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return {};
  }
  std::string req = std::string("GET ") + path + " HTTP/1.1\r\nHost: test\r\nConnection: close\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  std::string buf;
  buf.resize(4096);
  ssize_t received = ::recv(fd, buf.data(), buf.size(), 0);
  if (received > 0) {
    buf.resize(static_cast<size_t>(received));
  } else {
    buf.clear();
  }
  ::close(fd);
  return buf;
}
}  // namespace

TEST(HttpMultiReusePort, TwoServersBindSamePort) {
  uint16_t port = 18234;  // random high port
  aeronet::HttpServer serverA(aeronet::ServerConfig{}.withPort(port).withReusePort());
  serverA.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body = "A";
    return resp;
  });

  aeronet::HttpServer serverB(aeronet::ServerConfig{}.withPort(port).withReusePort());
  serverB.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse resp;
    resp.body = "B";
    return resp;
  });

  std::promise<void> startedA;
  std::promise<void> startedB;

  std::thread tA([&] {
    startedA.set_value();
    serverA.run();
  });
  startedA.get_future().wait();
  std::thread tB([&] {
    startedB.set_value();
    serverB.run();
  });
  startedB.get_future().wait();

  // Give kernel a moment to establish both listening sockets
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::string resp1 = simpleGet("127.0.0.1", port, "/one");
  std::string resp2 = simpleGet("127.0.0.1", port, "/two");
  bool hasA = resp1.find('A') != std::string::npos || resp2.find('A') != std::string::npos;
  bool hasB = resp1.find('B') != std::string::npos || resp2.find('B') != std::string::npos;
  if (!(hasA && hasB)) {
    // try a few more connects
    for (int i = 0; i < 5 && !(hasA && hasB); ++i) {
      auto retryResp = simpleGet("127.0.0.1", port, "/retry");
      if (retryResp.find('A') != std::string::npos) {
        hasA = true;
      }
      if (retryResp.find('B') != std::string::npos) {
        hasB = true;
      }
    }
  }

  serverA.stop();
  serverB.stop();
  tA.join();
  tB.join();

  // At least one of the responses should contain body A and one body B
  // Because of hashing, both could come from same server but with two sequential connects
  // we expect distribution eventually, so tolerate the rare case of both identical by allowing either pattern
  EXPECT_TRUE(hasA);
  EXPECT_TRUE(hasB);
}
