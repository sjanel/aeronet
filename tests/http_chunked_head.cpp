#include <gtest/gtest.h>

#include <cerrno>
#include <string>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"
#include "test_server_fixture.hpp"

using namespace std::chrono_literals;

TEST(HttpChunked, DecodeBasic) {
  TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.setHandler([](const aeronet::HttpRequest& req) {
    return aeronet::HttpResponse(200).body(std::string("LEN=") + std::to_string(req.body().size()) + ":" +
                                           std::string(req.body()));
  });

  aeronet::test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /c HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
  aeronet::test::sendAll(fd, req);
  std::string resp = aeronet::test::recvUntilClosed(fd);
  ASSERT_NE(std::string::npos, resp.find("LEN=9:Wikipedia"));
}

TEST(HttpChunked, RejectTooLarge) {
  aeronet::HttpServerConfig cfg;
  cfg.withMaxBodyBytes(4);  // very small limit
  TestServer ts(cfg);
  auto port = ts.port();
  ts.server.setHandler([](const aeronet::HttpRequest& req) { return aeronet::HttpResponse(200).body(req.body()); });
  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();
  // Single 5-byte chunk exceeds limit 4
  std::string req =
      "POST /big HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nabcde\r\n0\r\n\r\n";
  aeronet::test::sendAll(fd, req);
  std::string resp = aeronet::test::recvUntilClosed(fd);
  ASSERT_NE(std::string::npos, resp.find("413"));
}

TEST(HttpHead, NoBodyReturned) {
  TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.setHandler([](const aeronet::HttpRequest& req) {
    return aeronet::HttpResponse(200).body(std::string("DATA-") + std::string(req.path()));
  });
  aeronet::test::ClientConnection cnx(port);
  int fd = cnx.fd();
  std::string req = "HEAD /head HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  aeronet::test::sendAll(fd, req);
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
  TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.setHandler([](const aeronet::HttpRequest& req) { return aeronet::HttpResponse(200).body(req.body()); });
  aeronet::test::ClientConnection cnx(port);
  std::string headers =
      "POST /e HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  auto hs = ::send(cnx.fd(), headers.data(), headers.size(), 0);
  ASSERT_EQ(hs, static_cast<decltype(hs)>(headers.size()));
  // read interim 100
  char buf[128];
  auto firstRead = ::recv(cnx.fd(), buf, sizeof(buf), 0);
  std::string interim(buf, buf + (firstRead > 0 ? firstRead : 0));
  ASSERT_NE(std::string::npos, interim.find("100 Continue"));
  std::string body = "hello";
  auto bs = ::send(cnx.fd(), body.data(), body.size(), 0);
  ASSERT_EQ(bs, static_cast<decltype(hs)>(body.size()));

  aeronet::test::sendAll(cnx.fd(), "");
  std::string full = interim + aeronet::test::recvUntilClosed(cnx.fd());

  ASSERT_NE(std::string::npos, full.find("hello"));
}
