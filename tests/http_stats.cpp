#include <gtest/gtest.h>

#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "test_util.hpp"

using namespace aeronet;

TEST(HttpStats, BasicCountersIncrement) {
  ServerConfig cfg;
  cfg.withPort(0).withMaxRequestsPerConnection(5);
  HttpServer server(cfg);
  server.setHandler([](const HttpRequest&) {
    HttpResponse r;
    r.statusCode = 200;
    r.reason = "OK";
    r.body = "hello";
    r.contentType = "text/plain";
    return r;
  });

  // Run server in a thread
  std::atomic<bool> done{false};
  std::thread th([&]() { server.runUntil([&]() { return done.load(); }, std::chrono::milliseconds{50}); });
  // Wait until the server run loop has actually entered (isRunning) and port captured.
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Single request
  int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_GE(sock, 0);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(server.port());
  // Retry connect for a short window in case we raced right after isRunning() flipped.
  bool connected = false;
  for (int attempt = 0; attempt < 50; ++attempt) {
    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
      connected = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(connected) << "Failed to connect to ephemeral port " << server.port();
  std::string req = "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  ASSERT_EQ(::send(sock, req.data(), req.size(), 0), (ssize_t)req.size());
  char buf[1024];
  ssize_t receivedBytes = ::recv(sock, buf, sizeof(buf), 0);
  ASSERT_GT(receivedBytes, 0);
  ::close(sock);

  done.store(true);
  th.join();

  auto st = server.stats();
  EXPECT_GT(st.totalBytesQueued, 0U);  // headers+body accounted
  EXPECT_GT(st.totalBytesWrittenImmediate + st.totalBytesWrittenFlush, 0U);
  EXPECT_GE(st.maxConnectionOutboundBuffer, 0U);
  EXPECT_GE(st.flushCycles, 0U);
}
