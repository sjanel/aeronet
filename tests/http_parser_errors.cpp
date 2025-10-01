#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/test_util.hpp"
#include "test_server_fixture.hpp"

using namespace std::chrono_literals;

namespace {
struct Capture {
  std::mutex m;
  std::vector<aeronet::http::StatusCode> errors;
  void push(aeronet::http::StatusCode err) {
    std::scoped_lock lk(m);
    errors.push_back(err);
  }
};
}  // namespace

TEST(HttpParserErrors, InvalidVersion505) {
  TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  Capture cap;
  ts.server.setParserErrorCallback([&](aeronet::http::StatusCode err) { cap.push(err); });
  ts.server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse(200); });
  aeronet::test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string bad = "GET / HTTP/9.9\r\nHost: x\r\nConnection: close\r\n\r\n";  // unsupported version
  aeronet::test::sendAll(fd, bad);
  std::string resp = aeronet::test::recvUntilClosed(fd);
  ts.stop();
  ASSERT_NE(std::string::npos, resp.find("505")) << resp;
  bool seen = false;
  {
    std::scoped_lock lk(cap.m);
    for (auto err : cap.errors) {
      if (err == aeronet::http::StatusCodeHTTPVersionNotSupported) {
        seen = true;
      }
    }
  }
  ASSERT_TRUE(seen);
}

TEST(HttpParserErrors, Expect100OnlyWithBody) {
  TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse(200); });
  aeronet::test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  // zero length with Expect should NOT produce 100 Continue
  std::string zero =
      "POST /z HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  aeronet::test::sendAll(fd, zero);
  std::string respZero = aeronet::test::recvUntilClosed(fd);
  ASSERT_EQ(std::string::npos, respZero.find("100 Continue"));
  // non-zero length with Expect should produce interim 100 then 200
  aeronet::test::ClientConnection clientConnection2(port);
  int fd2 = clientConnection2.fd();
  ASSERT_GE(fd2, 0);
  std::string post =
      "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\nHELLO";
  aeronet::test::sendAll(fd2, post);
  std::string resp = aeronet::test::recvUntilClosed(fd2);
  ts.stop();
  ASSERT_NE(std::string::npos, resp.find("100 Continue"));
  ASSERT_NE(std::string::npos, resp.find("200"));
}

// Fuzz-ish incremental chunk framing with random chunk sizes & boundaries.
TEST(HttpParserErrors, ChunkIncrementalFuzz) {
  TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.setHandler([](const aeronet::HttpRequest& req) { return aeronet::HttpResponse(200).body(req.body()); });

  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> sizeDist(1, 15);
  std::string original;
  aeronet::test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string head = "POST /f HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  aeronet::test::sendAll(fd, head);
  // send 5 random chunks
  for (int i = 0; i < 5; ++i) {
    int sz = sizeDist(rng);
    std::string chunk(static_cast<std::size_t>(sz), static_cast<char>('a' + (i % 26)));
    original += chunk;
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%x", sz);
    std::string frame = std::string(hex) + "\r\n" + chunk + "\r\n";
    std::size_t pos = 0;
    while (pos < frame.size()) {
      std::size_t rem = frame.size() - pos;
      std::size_t slice = std::min<std::size_t>(1 + (rng() % 3), rem);
      aeronet::test::sendAll(fd, frame.substr(pos, slice));
      pos += slice;
      std::this_thread::sleep_for(1ms);
    }
  }
  // terminating chunk
  aeronet::test::sendAll(fd, "0\r\n\r\n");
  std::string resp = aeronet::test::recvUntilClosed(fd);
  ts.stop();
  ASSERT_NE(std::string::npos, resp.find("200"));
  ASSERT_NE(std::string::npos, resp.find(original.substr(0, 3))) << resp;  // sanity partial check
}
