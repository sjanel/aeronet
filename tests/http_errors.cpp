#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <thread>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "test_server_fixture.hpp"
#include "test_util.hpp"

using namespace std::chrono_literals;

namespace {
std::string sendAndCollect(uint16_t port, const std::string& raw) {
  ClientConnection clientConnection(port);
  int fd = clientConnection.fd();

  tu_sendAll(fd, raw);
  std::string out = tu_recvUntilClosed(fd);
  return out;
}
}  // namespace

struct ErrorCase {
  const char* name;
  const char* request;
  const char* expectedStatus;  // substring (e.g. "400", "505")
};

class HttpErrorParamTest : public ::testing::TestWithParam<ErrorCase> {};

TEST_P(HttpErrorParamTest, EmitsExpectedStatus) {
  TestServer ts(aeronet::HttpServerConfig{});
  ts.server.setHandler([](const aeronet::HttpRequest&) { return aeronet::HttpResponse{}; });
  const auto& param = GetParam();
  std::string resp = sendAndCollect(ts.port(), param.request);
  ASSERT_NE(std::string::npos, resp.find(param.expectedStatus)) << "Case=" << param.name << "\nResp=" << resp;
}

INSTANTIATE_TEST_SUITE_P(
    HttpErrors, HttpErrorParamTest,
    ::testing::Values(ErrorCase{"MalformedRequestLine", "GETONLY\r\n\r\n", "400"},
                      ErrorCase{"VersionNotSupported", "GET /test HTTP/2.0\r\nHost: x\r\n\r\n", "505"},
                      ErrorCase{"UnsupportedTransferEncoding",
                                "POST /u HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\nConnection: close\r\n\r\n",
                                "501"},
                      ErrorCase{"ContentLengthTransferEncodingConflict",
                                "POST /c HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nTransfer-Encoding: "
                                "chunked\r\nConnection: close\r\n\r\nhello",
                                "400"}));

TEST(HttpKeepAlive10, DefaultCloseWithoutHeader) {
  TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse response;
    response.body = "ok";
    return response;
  });
  // HTTP/1.0 without Connection: keep-alive should close
  ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req = "GET /h HTTP/1.0\r\nHost: x\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  char buf[512];
  std::string resp;
  ssize_t bytesRead;
  while ((bytesRead = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
    resp.append(buf, buf + bytesRead);
  }
  ASSERT_NE(std::string::npos, resp.find("Connection: close"));
  // Second request should not yield another response (connection closed). We attempt to read after sending.
  std::string req2 = "GET /h2 HTTP/1.0\r\nHost: x\r\n\r\n";
  ::send(fd, req2.data(), req2.size(), 0);
  char buf2[256];
  auto n2 = ::recv(fd, buf2, sizeof(buf2), 0);
  EXPECT_LE(n2, 0);
  ts.stop();
}

TEST(HttpKeepAlive10, OptInWithHeader) {
  TestServer ts(aeronet::HttpServerConfig{});
  auto port = ts.port();
  ts.server.setHandler([](const aeronet::HttpRequest&) {
    aeronet::HttpResponse response;
    response.body = "ok";
    return response;
  });
  ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req = "GET /h HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  std::string first;
  char buf[512];
  for (int i = 0; i < 50; ++i) {
    auto received = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (received > 0) {
      first.append(buf, buf + received);
    } else if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      std::this_thread::sleep_for(5ms);
      continue;
    } else {
      break;
    }
    if (first.find("\r\n\r\n") != std::string::npos) {
      break;  // got headers
    }
  }
  ASSERT_NE(std::string::npos, first.find("Connection: keep-alive"));
  std::string req2 = "GET /h2 HTTP/1.0\r\nHost: x\r\nConnection: keep-alive\r\n\r\n";
  ::send(fd, req2.data(), req2.size(), 0);
  std::string second;
  for (int i = 0; i < 50; ++i) {
    auto received = ::recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
    if (received > 0) {
      second.append(buf, buf + received);
    } else if (received == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      std::this_thread::sleep_for(5ms);
      continue;
    } else {
      break;
    }
  }
  ASSERT_NE(std::string::npos, second.find("Connection: keep-alive"));
  ts.stop();
}
