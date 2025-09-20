#include <gtest/gtest.h>

#include <atomic>
#include <random>
#include <thread>
#include <vector>

#include "aeronet/server.hpp"
#include "test_util.hpp"
using namespace std::chrono_literals;

namespace {
struct Capture {
  std::mutex m;
  std::vector<aeronet::HttpServer::ParserError> errors;
  void push(aeronet::HttpServer::ParserError err) {
    std::lock_guard<std::mutex> lk(m);
    errors.push_back(err);
  }
};
}  // namespace

TEST(HttpParserErrors, InvalidVersion505) {
  uint16_t port = 18630;
  aeronet::HttpServer server(aeronet::ServerConfig{}.withPort(port));
  Capture cap;
  server.setParserErrorCallback([&](aeronet::HttpServer::ParserError err) { cap.push(err); });
  server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  std::thread th([&] { server.runUntil([] { return false; }, 25ms); });
  std::this_thread::sleep_for(50ms);
  int fd = tu_connect(port);
  ASSERT_GE(fd, 0);
  std::string bad = "GET / HTTP/9.9\r\nHost: x\r\nConnection: close\r\n\r\n";  // unsupported version
  tu_sendAll(fd, bad);
  std::string resp = tu_recvUntilClosed(fd);
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("505")) << resp;
  bool seen = false;
  {
    std::lock_guard<std::mutex> lk(cap.m);
    for (auto err : cap.errors) {
      if (err == aeronet::HttpServer::ParserError::VersionUnsupported) {
        seen = true;
      }
    }
  }
  ASSERT_TRUE(seen);
}

TEST(HttpParserErrors, Expect100OnlyWithBody) {
  uint16_t port = 18631;
  aeronet::HttpServer server(aeronet::ServerConfig{}.withPort(port));
  server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  std::thread th([&] { server.runUntil([] { return false; }, 25ms); });
  std::this_thread::sleep_for(50ms);
  int fd = tu_connect(port);
  ASSERT_GE(fd, 0);
  // zero length with Expect should NOT produce 100 Continue
  std::string zero =
      "POST /z HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  tu_sendAll(fd, zero);
  std::string respZero = tu_recvUntilClosed(fd);
  ASSERT_EQ(std::string::npos, respZero.find("100 Continue"));
  // non-zero length with Expect should produce interim 100 then 200
  int fd2 = tu_connect(port);
  ASSERT_GE(fd2, 0);
  std::string post =
      "POST /p HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\nHELLO";
  tu_sendAll(fd2, post);
  std::string resp = tu_recvUntilClosed(fd2);
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("100 Continue"));
  ASSERT_NE(std::string::npos, resp.find("200"));
}

// Fuzz-ish incremental chunk framing with random chunk sizes & boundaries.
TEST(HttpParserErrors, ChunkIncrementalFuzz) {
  uint16_t port = 18632;
  aeronet::HttpServer server(aeronet::ServerConfig{}.withPort(port));
  server.setHandler([](const aeronet::HttpRequest& req) {
    aeronet::HttpResponse respObj;
    respObj.body = std::string(req.body);
    return respObj;
  });
  std::thread th([&] { server.runUntil([] { return false; }, 25ms); });
  std::this_thread::sleep_for(50ms);
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> sizeDist(1, 15);
  std::string original;
  int fd = tu_connect(port);
  ASSERT_GE(fd, 0);
  std::string head = "POST /f HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  tu_sendAll(fd, head);
  // send 5 random chunks
  for (int i = 0; i < 5; ++i) {
    int sz = sizeDist(rng);
    std::string chunk(sz, static_cast<char>('a' + (i % 26)));
    original += chunk;
    char hex[16];
    std::snprintf(hex, sizeof(hex), "%x", sz);
    std::string frame = std::string(hex) + "\r\n" + chunk + "\r\n";
    size_t pos = 0;
    while (pos < frame.size()) {
      size_t rem = frame.size() - pos;
      size_t slice = std::min<size_t>(1 + (rng() % 3), rem);
      tu_sendAll(fd, frame.substr(pos, slice));
      pos += slice;
      std::this_thread::sleep_for(1ms);
    }
  }
  // terminating chunk
  tu_sendAll(fd, "0\r\n\r\n");
  std::string resp = tu_recvUntilClosed(fd);
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("200"));
  ASSERT_NE(std::string::npos, resp.find(original.substr(0, 3))) << resp;  // sanity partial check
}
