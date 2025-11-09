#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

namespace {
test::TestServer ts(HttpServerConfig{}, RouterConfig{}, std::chrono::milliseconds{5});
auto port = ts.port();
}  // namespace

TEST(HttpChunked, DecodeBasic) {
  ts.router().setDefault([](const HttpRequest& req) {
    return HttpResponse(http::StatusCodeOK)
        .body(std::string("LEN=") + std::to_string(req.body().size()) + ":" + std::string(req.body()));
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /c HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("LEN=9:Wikipedia"));
}

TEST(HttpHead, NoBodyReturned) {
  ts.router().setDefault([](const HttpRequest& req) {
    return HttpResponse(http::StatusCodeOK).body(std::string("DATA-") + std::string(req.path()));
  });
  test::ClientConnection cnx(port);
  int fd = cnx.fd();
  std::string req = "HEAD /head HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  // Should have Content-Length header referencing length of would-be body (which is 10: DATA-/head)
  ASSERT_TRUE(resp.contains("Content-Length: 10"));
  // And not actually contain DATA-/head bytes after header terminator
  auto hdrEnd = resp.find(http::DoubleCRLF);
  ASSERT_NE(std::string::npos, hdrEnd);
  std::string after = resp.substr(hdrEnd + http::DoubleCRLF.size());
  ASSERT_TRUE(after.empty());
}

TEST(HttpExpect, ContinueFlow) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxBodyBytes(5); });
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(http::StatusCodeOK).body(req.body()); });
  test::ClientConnection cnx(port);
  auto fd = cnx.fd();
  std::string headers =
      "POST /e HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, headers);
  // Read the interim 100 Continue response using the helper with a short timeout.
  std::string interim = test::recvWithTimeout(fd, 200ms);
  ASSERT_TRUE(interim.contains("100 Continue"));
  std::string body = "hello";
  // Use sendAll for robust writes
  test::sendAll(cnx.fd(), body);

  // Ensure any remaining bytes are collected until the peer closes
  std::string full = interim + test::recvUntilClosed(cnx.fd());

  ASSERT_TRUE(full.contains("hello"));
}

TEST(HttpChunked, RejectTooLarge) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.withMaxBodyBytes(4);  // very small limit
  });
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(http::StatusCodeOK).body(req.body()); });
  test::ClientConnection cnx(port);
  int fd = cnx.fd();
  // Single 5-byte chunk exceeds limit 4
  std::string req =
      "POST /big HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nabcde\r\n0\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("413"));
}
