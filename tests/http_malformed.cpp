#include <gtest/gtest.h>

#include <string>
#include <thread>

#include "aeronet/server.hpp"
#include "test_util.hpp"
using namespace std::chrono_literals;

namespace {
std::string sendRaw(uint16_t port, const std::string& raw) {
  int fd = tu_connect(port);
  if (fd < 0) {
    return {};
  }
  tu_sendAll(fd, raw);
  std::string resp = tu_recvWithTimeout(fd, 300ms);
  // server may close depending on error severity
  return resp;
}
}  // anonymous namespace

TEST(HttpMalformed, MissingSpacesInRequestLine) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  std::thread th([&] { server.runUntil([] { return false; }, 25ms); });
  std::this_thread::sleep_for(50ms);
  std::string resp = sendRaw(port, "GET/abcHTTP/1.1\r\nHost: x\r\n\r\n");
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("400")) << resp;
}

TEST(HttpMalformed, OversizedHeaders) {
  aeronet::ServerConfig cfg;
  cfg.withMaxHeaderBytes(64);
  aeronet::HttpServer server(cfg);
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  std::thread th([&] { server.runUntil([] { return false; }, 25ms); });
  std::this_thread::sleep_for(50ms);
  std::string big(200, 'A');
  std::string raw = "GET / HTTP/1.1\r\nHost: x\r\nX-Big: " + big + "\r\n\r\n";
  std::string resp = sendRaw(port, raw);
  server.stop();
  th.join();
  ASSERT_NE(std::string::npos, resp.find("431")) << resp;
}

TEST(HttpMalformed, BadChunkExtensionHex) {
  aeronet::HttpServer server(aeronet::ServerConfig{});
  uint16_t port = server.port();
  server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  std::thread th([&] { server.runUntil([] { return false; }, 25ms); });
  std::this_thread::sleep_for(50ms);
  // Transfer-Encoding with invalid hex char 'Z'
  std::string raw = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\n";  // incomplete + invalid
  std::string resp = sendRaw(port, raw);
  server.stop();
  th.join();
  // Expect no 200 OK; either empty (waiting for more) or eventually 413/400 once completed; we at least assert not 200
  ASSERT_EQ(std::string::npos, resp.find("200 OK"));
}
