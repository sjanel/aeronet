#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

namespace {
test::TestServer ts(HttpServerConfig{});
}

struct ErrorCase {
  const char* name;
  const char* request;
  const char* expectedStatus;  // substring (e.g. "400", "505")
};

class HttpErrorParamTest : public ::testing::TestWithParam<ErrorCase> {};

TEST_P(HttpErrorParamTest, EmitsExpectedStatus) {
  ts.server.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });
  const auto& param = GetParam();
  std::string resp = test::sendAndCollect(ts.port(), param.request);
  ASSERT_TRUE(resp.contains(param.expectedStatus)) << "Case=" << param.name << "\nResp=" << resp;
}

INSTANTIATE_TEST_SUITE_P(
    HttpErrors, HttpErrorParamTest,
    ::testing::Values(ErrorCase{"MalformedRequestLine", "GETONLYNOPATH\r\n\r\n", "400"},
                      ErrorCase{"VersionNotSupported", "GET /test HTTP/2.0\r\nHost: x\r\n\r\n", "505"},
                      ErrorCase{"UnsupportedTransferEncoding",
                                "POST /u HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\nConnection: close\r\n\r\n",
                                "501"},
                      ErrorCase{"ContentLengthTransferEncodingConflict",
                                "POST /c HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: "
                                "chunked\r\nConnection: close\r\n\r\nhello",
                                "400"}));

TEST(HttpKeepAlive10, DefaultCloseWithoutHeader) {
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest&) { return HttpResponse().body("ok"); });
  // HTTP/1.0 without Connection: keep-alive should close
  test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string_view req = "GET /h HTTP/1.0\r\nHost: x\r\n\r\n";
  ASSERT_TRUE(test::sendAll(fd, req));

  std::string resp = test::recvUntilClosed(fd);

  ASSERT_TRUE(resp.contains("Connection: close"));
  // Second request should not yield another response (connection closed). We attempt to read after sending.
  std::string_view req2 = "GET /h2 HTTP/1.0\r\nHost: x\r\n\r\n";
  ASSERT_TRUE(test::sendAll(fd, req2));
  // Expect no data (connection should be closed) -- use test helper which waits briefly
  auto n2 = test::recvWithTimeout(fd);
  EXPECT_TRUE(n2.empty());
}

TEST(HttpKeepAlive10, OptInWithHeader) {
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest&) { return HttpResponse().body("ok"); });
  test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string_view req = "GET /h HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  ASSERT_TRUE(test::sendAll(fd, req));
  std::string first = test::recvWithTimeout(fd);
  ASSERT_TRUE(first.contains("Connection: keep-alive"));
  std::string_view req2 = "GET /h2 HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  ASSERT_TRUE(test::sendAll(fd, req2));
  std::string second = test::recvWithTimeout(fd);
  ASSERT_TRUE(second.contains("Connection: keep-alive"));
}

namespace {
std::string sendRaw(uint16_t port, std::string_view raw) {
  test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  test::sendAll(fd, raw);
  std::string resp = test::recvWithTimeout(fd, 300ms);
  // server may close depending on error severity
  return resp;
}
}  // anonymous namespace

TEST(HttpMalformed, MissingSpacesInRequestLine) {
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });
  std::string resp = sendRaw(port, "GET/abcHTTP/1.1\r\nHost: x\r\n\r\n");
  ASSERT_TRUE(resp.contains("400")) << resp;
}

TEST(HttpMalformed, OversizedHeaders) {
  HttpServerConfig cfg;
  cfg.withMaxHeaderBytes(128);
  HttpServer server(cfg);
  auto port = server.port();
  server.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });
  std::jthread th([&] { server.run(); });
  std::this_thread::sleep_for(50ms);
  std::string big(200, 'A');
  std::string raw = "GET / HTTP/1.1\r\nHost: x\r\nX-Big: " + big + "\r\n\r\n";
  std::string resp = sendRaw(port, raw);
  server.stop();
  ASSERT_TRUE(resp.contains("431")) << resp;
}

TEST(HttpMalformed, BadChunkExtensionHex) {
  auto port = ts.port();
  ts.server.router().setDefault([](const HttpRequest&) { return HttpResponse(http::StatusCodeOK); });

  // Transfer-Encoding with invalid hex char 'Z'
  std::string raw = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\n";  // incomplete + invalid
  std::string resp = sendRaw(port, raw);
  // Expect no 200 OK; either empty (waiting for more) or eventually 413/400 once completed; we at least assert not 200
  ASSERT_FALSE(resp.contains("200 OK"));
}
