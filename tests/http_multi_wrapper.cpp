#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/multi-http-server.hpp"
#include "test_raw_get.hpp"

TEST(MultiHttpServer, BasicStartAndServe) {
  const int threads = 3;
  aeronet::MultiHttpServer multi(aeronet::HttpServerConfig{}.withReusePort(), threads);
  multi.setHandler([]([[maybe_unused]] const aeronet::HttpRequest& req) {
    aeronet::HttpResponse resp;
    resp.body(std::string("Hello ")); /* path not exposed directly */
    return resp;
  });
  multi.start();
  auto port = multi.port();
  ASSERT_GT(port, 0);
  // allow sockets to be fully listening
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  std::string r1;
  std::string r2;
  test_helpers::rawGet(port, "/one", r1);
  test_helpers::rawGet(port, "/two", r2);
  EXPECT_NE(std::string::npos, r1.find("Hello"));
  EXPECT_NE(std::string::npos, r2.find("Hello"));

  auto stats = multi.stats();
  EXPECT_EQ(stats.per.size(), static_cast<std::size_t>(threads));

  multi.stop();
}
