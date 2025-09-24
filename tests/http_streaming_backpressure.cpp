#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "socket.hpp"
#include "test_server_fixture.hpp"

using namespace aeronet;

TEST(StreamingBackpressure, LargeBodyQueues) {
  ServerConfig cfg;
  cfg.port = 0;
  cfg.enableKeepAlive = false;                                       // simplicity
  cfg.maxOutboundBufferBytes = static_cast<std::size_t>(64 * 1024);  // assume default maybe larger
  TestServer ts(cfg);
  std::size_t total = static_cast<std::size_t>(512 * 1024);  // 512 KB
  ts.server.setStreamingHandler([&]([[maybe_unused]] const HttpRequest& req, HttpResponseWriter& writer) {
    writer.setStatus(200, "OK");
    std::string chunk(8192, 'x');
    std::size_t sent = 0;
    while (sent < total) {
      writer.write(chunk);
      sent += chunk.size();
    }
    writer.end();
  });
  auto port = ts.port();
  aeronet::Socket sock(aeronet::Socket::Type::STREAM);
  int fd = sock.fd();
  ASSERT_GE(fd, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  ASSERT_EQ(::connect(fd, (sockaddr*)&addr, sizeof(addr)), 0);
  std::string req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
  ASSERT_EQ(::send(fd, req.data(), req.size(), 0), req.size());
  // just read some bytes to allow flush cycles
  char buf[4096];
  for (int i = 0; i < 10; i++) {
    auto rcv = ::recv(fd, buf, sizeof(buf), 0);
    if (rcv <= 0) {
      // Connection might close early; break to avoid ASSERT on expected close after full send.
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ts.stop();
}
