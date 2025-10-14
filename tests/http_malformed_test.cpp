#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;

namespace {
std::string sendRaw(uint16_t port, std::string_view raw) {
  aeronet::test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  aeronet::test::sendAll(fd, raw);
  std::string resp = aeronet::test::recvWithTimeout(fd, 300ms);
  // server may close depending on error severity
  return resp;
}
}  // anonymous namespace

TEST(HttpMalformed, MissingSpacesInRequestLine) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  auto port = server.port();
  server.router().setDefault([](const aeronet::HttpRequest&) { return aeronet::HttpResponse(200); });
  std::jthread th([&] { server.runUntil([] { return false; }); });
  std::this_thread::sleep_for(50ms);
  std::string resp = sendRaw(port, "GET/abcHTTP/1.1\r\nHost: x\r\n\r\n");
  server.stop();
  ASSERT_TRUE(resp.contains("400")) << resp;
}

TEST(HttpMalformed, OversizedHeaders) {
  aeronet::HttpServerConfig cfg;
  cfg.withMaxHeaderBytes(64);
  aeronet::HttpServer server(cfg);
  auto port = server.port();
  server.router().setDefault([](const aeronet::HttpRequest&) { return aeronet::HttpResponse(200); });
  std::jthread th([&] { server.runUntil([] { return false; }); });
  std::this_thread::sleep_for(50ms);
  std::string big(200, 'A');
  std::string raw = "GET / HTTP/1.1\r\nHost: x\r\nX-Big: " + big + "\r\n\r\n";
  std::string resp = sendRaw(port, raw);
  server.stop();
  ASSERT_TRUE(resp.contains("431")) << resp;
}

TEST(HttpMalformed, BadChunkExtensionHex) {
  aeronet::HttpServer server(aeronet::HttpServerConfig{});
  auto port = server.port();
  server.router().setDefault([](const aeronet::HttpRequest&) { return aeronet::HttpResponse(200); });
  std::jthread th([&] { server.runUntil([] { return false; }); });
  std::this_thread::sleep_for(50ms);
  // Transfer-Encoding with invalid hex char 'Z'
  std::string raw = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\n";  // incomplete + invalid
  std::string resp = sendRaw(port, raw);
  server.stop();
  // Expect no 200 OK; either empty (waiting for more) or eventually 413/400 once completed; we at least assert not 200
  ASSERT_FALSE(resp.contains("200 OK"));
}
