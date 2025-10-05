#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/multi-http-server.hpp"

using namespace std::chrono_literals;

TEST(MultiHttpServer, RapidStartStopCycles) {
  aeronet::HttpServerConfig cfg;
  cfg.withReusePort();
  // Keep cycles modest to avoid lengthening normal test runtime too much; adjust if needed.
  for (int statePos = 0; statePos < 200; ++statePos) {
    aeronet::MultiHttpServer multi(cfg);
    multi.setHandler([]([[maybe_unused]] const aeronet::HttpRequest& req) {
      aeronet::HttpResponse resp;
      resp.body("S");
      return resp;
    });
    multi.start();
    ASSERT_TRUE(multi.isRunning());
    // Short dwell to allow threads to enter run loop.
    std::this_thread::sleep_for(2ms);
    multi.stop();
    EXPECT_FALSE(multi.isRunning());
  }
}
