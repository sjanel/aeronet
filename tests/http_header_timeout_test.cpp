#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

TEST(HttpHeaderTimeout, SlowHeadersConnectionClosed) {
  HttpServerConfig cfg;
  std::chrono::milliseconds readTimeout = std::chrono::milliseconds{50};
  cfg.withPort(0).withHeaderReadTimeout(readTimeout);
  // Use a short poll interval so the server's periodic maintenance (which enforces
  // header read timeouts) runs promptly even when the test runner is under heavy load.
  // This avoids flakiness when the whole test suite is executed in parallel.
  aeronet::test::TestServer ts(cfg, std::chrono::milliseconds{5});
  ts.server.router().setDefault([](const HttpRequest&) {
    return aeronet::HttpResponse(200, "OK").body("hi").contentType(aeronet::http::ContentTypeTextPlain);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  test::ClientConnection cnx(ts.port());
  int fd = cnx.fd();
  ASSERT_GE(fd, 0) << "connect failed";
  // Send only method token slowly using test helpers
  std::string_view part1 = "GET /";  // incomplete, no version yet
  ASSERT_TRUE(aeronet::test::sendAll(fd, part1, std::chrono::milliseconds{500}));
  std::this_thread::sleep_for(readTimeout + std::chrono::milliseconds{5});
  // Attempt to finish request
  std::string_view rest = " HTTP/1.1\r\nHost: x\r\n\r\n";
  [[maybe_unused]] bool sent_ok = aeronet::test::sendAll(fd, rest, std::chrono::milliseconds{500});
  // kernel may still accept bytes, server should close shortly after detecting timeout

  // Attempt to read response; expect either empty (no response) or nothing meaningful (no 200 OK)
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  std::string resp = aeronet::test::recvWithTimeout(fd, std::chrono::milliseconds{100});
  if (!resp.empty()) {
    // Should not have produced a 200 OK response because headers were never completed before timeout
    EXPECT_FALSE(resp.contains("200 OK")) << resp;
  }
}
