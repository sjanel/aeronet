#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace aeronet;

TEST(StreamingBackpressure, LargeBodyQueues) {
  HttpServerConfig cfg;
  cfg.enableKeepAlive = false;                                       // simplicity
  cfg.maxOutboundBufferBytes = static_cast<std::size_t>(64 * 1024);  // assume default maybe larger
  aeronet::test::TestServer ts(cfg);
  std::size_t total = static_cast<std::size_t>(512 * 1024);  // 512 KB
  ts.server.router().setDefault([&]([[maybe_unused]] const HttpRequest& req, HttpResponseWriter& writer) {
    writer.statusCode(200);
    std::string chunk(8192, 'x');
    std::size_t sent = 0;
    while (sent < total) {
      writer.writeBody(chunk);
      sent += chunk.size();
    }
    writer.end();
  });
  auto port = ts.port();
  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();
  std::string req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
  EXPECT_TRUE(aeronet::test::sendAll(fd, req));

  auto data = aeronet::test::recvUntilClosed(fd);
  EXPECT_TRUE(data.starts_with("HTTP/1.1 200"));
}
