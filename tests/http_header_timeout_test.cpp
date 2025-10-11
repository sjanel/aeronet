#include <gtest/gtest.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstring>
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
  aeronet::test::TestServer ts(cfg);
  ts.server.router().setDefault([](const HttpRequest&) {
    return aeronet::HttpResponse(200, "OK").body("hi").contentType(aeronet::http::ContentTypeTextPlain);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  test::ClientConnection cnx(ts.port());
  int fd = cnx.fd();
  ASSERT_GE(fd, 0) << "connect failed";
  // Send only method token slowly
  const char* part1 = "GET /";  // incomplete, no version yet
  ASSERT_GT(::send(fd, part1, std::strlen(part1), 0), 0) << strerror(errno);
  std::this_thread::sleep_for(readTimeout + std::chrono::milliseconds{5});
  // Attempt to finish request
  const char* rest = " HTTP/1.1\r\nHost: x\r\n\r\n";
  [[maybe_unused]] auto sent = ::send(fd, rest, std::strlen(rest), 0);
  // Kernel may still accept bytes, but server should close shortly after detecting timeout.

  // Attempt to read response; expect either 0 (closed) or nothing meaningful (no 200 OK)
  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  char buf[256];
  auto rcv = ::recv(fd, buf, sizeof(buf), 0);
  // If timeout occurred before headers complete, server closes without response (rcv==0) or ECONNRESET
  if (rcv > 0) {
    std::string_view sv(buf, static_cast<std::string_view::size_type>(rcv));
    // Should not have produced a 200 OK response because headers were never completed before timeout
    EXPECT_EQ(sv.find("200 OK"), std::string_view::npos) << sv;
  }
  ts.stop();
}
