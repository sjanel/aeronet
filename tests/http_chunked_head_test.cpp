#include <gtest/gtest.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstddef>
#include <string>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;

TEST(HttpChunked, DecodeBasic) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const aeronet::HttpRequest& req) {
    return aeronet::HttpResponse(200).body(std::string("LEN=") + std::to_string(req.body().size()) + ":" +
                                           std::string(req.body()));
  });

  aeronet::test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /c HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
  EXPECT_TRUE(aeronet::test::sendAll(fd, req));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  ASSERT_NE(std::string::npos, resp.find("LEN=9:Wikipedia"));
}

TEST(HttpChunked, RejectTooLarge) {
  aeronet::HttpServerConfig cfg;
  cfg.withMaxBodyBytes(4);  // very small limit
  aeronet::test::TestServer ts(cfg);
  auto port = ts.port();
  ts.server.router().setDefault(
      [](const aeronet::HttpRequest& req) { return aeronet::HttpResponse(200).body(req.body()); });
  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();
  // Single 5-byte chunk exceeds limit 4
  std::string req =
      "POST /big HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nabcde\r\n0\r\n\r\n";
  EXPECT_TRUE(aeronet::test::sendAll(fd, req));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  ASSERT_NE(std::string::npos, resp.find("413"));
}

TEST(HttpHead, NoBodyReturned) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const aeronet::HttpRequest& req) {
    return aeronet::HttpResponse(200).body(std::string("DATA-") + std::string(req.path()));
  });
  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();
  std::string req = "HEAD /head HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  EXPECT_TRUE(aeronet::test::sendAll(fd, req));
  std::string resp = aeronet::test::recvUntilClosed(fd);
  // Should have Content-Length header referencing length of would-be body (which is 10: DATA-/head)
  ASSERT_NE(std::string::npos, resp.find("Content-Length: 10"));
  // And not actually contain DATA-/head bytes after header terminator
  auto hdrEnd = resp.find(aeronet::http::DoubleCRLF);
  ASSERT_NE(std::string::npos, hdrEnd);
  std::string after = resp.substr(hdrEnd + aeronet::http::DoubleCRLF.size());
  ASSERT_TRUE(after.empty());
}

TEST(HttpExpect, ContinueFlow) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault(
      [](const aeronet::HttpRequest& req) { return aeronet::HttpResponse(200).body(req.body()); });
  aeronet::test::ClientConnection cnx(port);
  auto fd = cnx.fd();
  std::string headers =
      "POST /e HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  EXPECT_TRUE(aeronet::test::sendAll(fd, headers));
  std::string interim;
  static constexpr std::size_t kChunkSize = 128;
  interim.resize_and_overwrite(kChunkSize, [fd](char* buf, [[maybe_unused]] std::size_t) {
    auto firstRead = ::recv(fd, buf, kChunkSize, 0);
    if (firstRead >= 0) {
      return static_cast<std::size_t>(firstRead);
    }
    return std::size_t{};
  });
  ASSERT_NE(std::string::npos, interim.find("100 Continue"));
  std::string body = "hello";
  auto bs = ::send(cnx.fd(), body.data(), body.size(), 0);
  ASSERT_EQ(bs, static_cast<decltype(headers.size())>(body.size()));

  EXPECT_TRUE(aeronet::test::sendAll(cnx.fd(), ""));
  std::string full = interim + aeronet::test::recvUntilClosed(cnx.fd());

  ASSERT_NE(std::string::npos, full.find("hello"));
}
