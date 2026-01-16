#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/request-task.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

namespace {
test::TestServer ts(HttpServerConfig{}, RouterConfig{}, std::chrono::milliseconds{5});
auto port = ts.port();
}  // namespace

TEST(HttpChunked, DecodeBasic) {
  ts.router().setDefault([](const HttpRequest& req) {
    return HttpResponse(http::StatusCodeOK)
        .body(std::string("LEN=") + std::to_string(req.body().size()) + ":" + std::string(req.body()));
  });

  test::ClientConnection sock(port);
  int fd = sock.fd();

  std::string req =
      "POST /c HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("LEN=9:Wikipedia"));
}

TEST(HttpHead, NoBodyReturned) {
  ts.router().setDefault(
      [](const HttpRequest& req) { return HttpResponse(std::string("DATA-") + std::string(req.path())); });
  test::ClientConnection cnx(port);
  int fd = cnx.fd();
  std::string req = "HEAD /head HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  // Should have Content-Length header referencing length of would-be body (which is 10: DATA-/head)
  ASSERT_TRUE(resp.contains(MakeHttp1HeaderLine(http::ContentLength, "10")));
  // And not actually contain DATA-/head bytes after header terminator
  auto hdrEnd = resp.find(http::DoubleCRLF);
  ASSERT_NE(std::string::npos, hdrEnd);
  std::string after = resp.substr(hdrEnd + http::DoubleCRLF.size());
  ASSERT_TRUE(after.empty());
}

TEST(HttpExpect, ContinueFlow) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxBodyBytes(5); });
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });
  test::ClientConnection cnx(port);
  auto fd = cnx.fd();
  std::string headers =
      "POST /e HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, headers);
  // Read the interim 100 Continue response using the helper with a short timeout.
  std::string interim = test::recvWithTimeout(fd, 200ms);  // NOLINT(misc-include-cleaner)
  ASSERT_TRUE(interim.contains("100 Continue"));
  std::string body = "hello";
  // Use sendAll for robust writes
  test::sendAll(cnx.fd(), body);

  // Ensure any remaining bytes are collected until the peer closes
  std::string full = interim + test::recvUntilClosed(cnx.fd());

  ASSERT_TRUE(full.contains("hello"));
}

TEST(HttpChunked, RejectTooLarge) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.withMaxBodyBytes(4);  // very small limit
  });
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });
  test::ClientConnection cnx(port);
  int fd = cnx.fd();
  // Single 5-byte chunk exceeds limit 4
  std::string req =
      "POST /big HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nabcde\r\n0\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("413"));
}

TEST(HttpAsync, FlushPendingResponseAfterBody) {
  // Handler completes immediately but body wasn't ready when started.
  ts.resetRouterAndGet().setPath(http::Method::POST, "/async-flush",
                                 []([[maybe_unused]] HttpRequest& req) -> RequestTask<HttpResponse> {
                                   // Return a response immediately; if the request body
                                   // wasn't ready the server will hold it as pending.
                                   co_return HttpResponse(http::StatusCodeOK).body("async-ok");
                                 });

  test::ClientConnection cnx(port);
  int fd = cnx.fd();

  // Send headers first without body so server marks async.needsBody=true
  std::string hdrs = "POST /async-flush HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, hdrs);

  // Give server a short moment to start handler and mark response pending.
  std::this_thread::sleep_for(20ms);  // NOLINT(misc-include-cleaner)

  // Now send the body which should trigger tryFlushPendingAsyncResponse and send the response.
  test::sendAll(fd, "hello");

  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("async-ok")) << resp;
}
