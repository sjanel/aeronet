#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <thread>

#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"

using namespace aeronet;

TEST(StreamingBackpressure, LargeBodyQueues) {
  ServerConfig cfg;
  cfg.port = 0;
  cfg.enableKeepAlive = false;             // simplicity
  cfg.maxOutboundBufferBytes = 64 * 1024;  // assume default maybe larger
  HttpServer server(cfg);
  size_t total = 512 * 1024;  // 512 KB
  server.setStreamingHandler([&](const HttpRequest& req, HttpResponseWriter& w) {
    w.setStatus(200, "OK");
    std::string chunk(8192, 'x');
    size_t sent = 0;
    while (sent < total) {
      w.write(chunk);
      sent += chunk.size();
    }
    w.end();
  });
  std::thread th([&] { server.runUntil([&] { return server.port() != 0; }); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(server.port());
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  ASSERT_EQ(::connect(fd, (sockaddr*)&addr, sizeof(addr)), 0);
  std::string req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
  ASSERT_EQ(::send(fd, req.data(), req.size(), 0), (ssize_t)req.size());
  // just read some bytes to allow flush cycles
  char buf[4096];
  for (int i = 0; i < 10; i++) {
    ::recv(fd, buf, sizeof(buf), 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ::close(fd);
  server.stop();
  th.join();
}
