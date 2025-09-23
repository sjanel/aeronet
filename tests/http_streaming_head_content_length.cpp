#include <gtest/gtest.h>
#include <sys/socket.h>

#include <string>
#include <string_view>

#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "test_server_fixture.hpp"
#include "test_util.hpp"

using namespace std::chrono_literals;

namespace {
void raw(auto port, const std::string& verb, std::string& out) {
  ClientConnection sock(port);
  int fd = sock.fd();
  std::string req = verb + " /len HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  auto sent = ::send(fd, req.data(), req.size(), 0);
  ASSERT_EQ(sent, static_cast<decltype(sent)>(req.size())) << "send partial";
  char buf[4096];
  out.clear();
  while (true) {
    auto bytesRead = ::recv(fd, buf, sizeof(buf), 0);
    if (bytesRead <= 0) {
      break;
    }
    out.append(buf, buf + bytesRead);
  }
}
}  // namespace

TEST(HttpStreamingHeadContentLength, HeadSuppressesBodyKeepsCL) {
  aeronet::ServerConfig cfg;
  cfg.withMaxRequestsPerConnection(2);
  TestServer ts(cfg);
  ts.server.setStreamingHandler(
      []([[maybe_unused]] const aeronet::HttpRequest& req, aeronet::HttpResponseWriter& writer) {
        writer.setStatus(200, "OK");
        // We set Content-Length even though we write body pieces; for HEAD the body must be suppressed but CL retained.
        static constexpr std::string_view body = "abcdef";  // length 6
        writer.setContentLength(body.size());
        writer.write(body.substr(0, 3));
        writer.write(body.substr(3));
        writer.end();
      });
  auto port = ts.port();
  std::string headResp;
  std::string getResp;
  raw(port, "HEAD", headResp);
  raw(port, "GET", getResp);
  ts.stop();

  ASSERT_NE(std::string::npos, headResp.find("HTTP/1.1 200"));
  ASSERT_NE(std::string::npos, headResp.find("Content-Length: 6\r\n"));
  // No chunked framing, no body.
  ASSERT_EQ(std::string::npos, headResp.find("abcdef"));
  ASSERT_EQ(std::string::npos, headResp.find("Transfer-Encoding: chunked"));
  // GET path: should carry body; since we set fixed length it should not be chunked.
  ASSERT_NE(std::string::npos, getResp.find("HTTP/1.1 200"));
  ASSERT_NE(std::string::npos, getResp.find("Content-Length: 6\r\n"));
  ASSERT_NE(std::string::npos, getResp.find("abcdef"));
  ASSERT_EQ(std::string::npos, getResp.find("Transfer-Encoding: chunked"));
}
