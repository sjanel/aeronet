#include <gtest/gtest.h>

#include <string>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;

struct ErrorCase {
  const char* name;
  const char* request;
  const char* expectedStatus;  // substring (e.g. "400", "505")
};

class HttpErrorParamTest : public ::testing::TestWithParam<ErrorCase> {};

TEST_P(HttpErrorParamTest, EmitsExpectedStatus) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  ts.server.router().setDefault([](const aeronet::HttpRequest&) { return aeronet::HttpResponse(200); });
  const auto& param = GetParam();
  std::string resp = aeronet::test::sendAndCollect(ts.port(), param.request);
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
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("ok"); });
  // HTTP/1.0 without Connection: keep-alive should close
  aeronet::test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string_view req = "GET /h HTTP/1.0\r\nHost: x\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(fd, req));

  std::string resp = aeronet::test::recvUntilClosed(fd);

  ASSERT_TRUE(resp.contains("Connection: close"));
  // Second request should not yield another response (connection closed). We attempt to read after sending.
  std::string_view req2 = "GET /h2 HTTP/1.0\r\nHost: x\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(fd, req2));
  // Expect no data (connection should be closed) -- use test helper which waits briefly
  auto n2 = aeronet::test::recvWithTimeout(fd);
  EXPECT_TRUE(n2.empty());
}

TEST(HttpKeepAlive10, OptInWithHeader) {
  aeronet::test::TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.router().setDefault([](const aeronet::HttpRequest&) { return aeronet::HttpResponse().body("ok"); });
  aeronet::test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string_view req = "GET /h HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(fd, req));
  std::string first = aeronet::test::recvWithTimeout(fd);
  ASSERT_TRUE(first.contains("Connection: keep-alive"));
  std::string_view req2 = "GET /h2 HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  ASSERT_TRUE(aeronet::test::sendAll(fd, req2));
  std::string second = aeronet::test::recvWithTimeout(fd);
  ASSERT_TRUE(second.contains("Connection: keep-alive"));
}
