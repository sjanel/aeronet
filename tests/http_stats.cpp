#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "test_http_client.hpp"

using namespace aeronet;

TEST(HttpStats, BasicCountersIncrement) {
  ServerConfig cfg;
  cfg.withMaxRequestsPerConnection(5);
  HttpServer server(cfg);
  server.setHandler([]([[maybe_unused]] const HttpRequest& req) {
    HttpResponse resp;
    resp.statusCode = 200;
    resp.reason = "OK";
    resp.body = "hello";
    resp.contentType = "text/plain";
    return resp;
  });

  // Run server in a thread
  std::atomic<bool> done{false};
  std::jthread th([&]() { server.runUntil([&]() { return done.load(); }, std::chrono::milliseconds{50}); });
  // Wait until the server run loop has actually entered (isRunning) and port captured.
  for (int i = 0; i < 200 && (!server.isRunning() || server.port() == 0); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Single request via throwing helper (adds timeout & size cap safety)
  auto resp = test_http_client::request_or_throw(server.port());
  ASSERT_NE(resp.find("200 OK"), std::string::npos);

  done.store(true);
  th.join();

  auto st = server.stats();
  EXPECT_GT(st.totalBytesQueued, 0U);  // headers+body accounted
  EXPECT_GT(st.totalBytesWrittenImmediate + st.totalBytesWrittenFlush, 0U);
  EXPECT_GE(st.maxConnectionOutboundBuffer, 0U);
  EXPECT_GE(st.flushCycles, 0U);
}
