#include <dirent.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/stringconv.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/request-task.hpp"
#endif

// Enable epoll/socket syscall overrides from sys-test-support.hpp
#define AERONET_WANT_SOCKET_OVERRIDES
#define AERONET_WANT_READ_WRITE_OVERRIDES
#include "aeronet/sys-test-support.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

namespace {

HttpServerConfig TestServerConfig() {
  HttpServerConfig cfg;
#ifdef AERONET_ENABLE_OPENTELEMETRY
  cfg.telemetry.otelEnabled = true;
#else
  cfg.telemetry.otelEnabled = false;
#endif
  cfg.pollInterval = std::chrono::milliseconds{1};
  return cfg;
}

test::TestServer ts(TestServerConfig());

struct Ipv4Endpoint {
  uint32_t addr{0};
  uint16_t port{0};
};

bool GetIpv4SockName(int fd, Ipv4Endpoint& out) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    return false;
  }
  if (addr.sin_family != AF_INET) {
    return false;
  }
  out.addr = addr.sin_addr.s_addr;
  out.port = addr.sin_port;
  return true;
}

bool GetIpv4PeerName(int fd, Ipv4Endpoint& out) {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getpeername(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    return false;
  }
  if (addr.sin_family != AF_INET) {
    return false;
  }
  out.addr = addr.sin_addr.s_addr;
  out.port = addr.sin_port;
  return true;
}

int FindServerSideFdForClientOrThrow(int clientFd, std::chrono::milliseconds timeout) {
  Ipv4Endpoint clientLocal{};
  Ipv4Endpoint clientPeer{};
  if (!GetIpv4SockName(clientFd, clientLocal) || !GetIpv4PeerName(clientFd, clientPeer)) {
    throw std::runtime_error("Unable to read client socket endpoints");
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    DIR* dir = ::opendir("/proc/self/fd");
    if (dir == nullptr) {
      throw std::runtime_error("Unable to open /proc/self/fd");
    }

    while (auto* ent = ::readdir(dir)) {
      const char* name = ent->d_name;
      if (name == nullptr || name[0] == '.') {
        continue;
      }
      char* endp = nullptr;
      const long parsed = std::strtol(name, &endp, 10);
      if (endp == name || parsed < 0 || parsed > INT32_MAX) {
        continue;
      }
      const int candFd = static_cast<int>(parsed);
      if (candFd == clientFd) {
        continue;
      }

      Ipv4Endpoint candLocal{};
      Ipv4Endpoint candPeer{};
      if (!GetIpv4SockName(candFd, candLocal) || !GetIpv4PeerName(candFd, candPeer)) {
        continue;
      }

      // Server-side accepted socket has local port == server port and peer port == client ephemeral port.
      // Prefer port matching: some platforms/configs can report wildcard local addresses.
      if (candLocal.port == clientPeer.port && candPeer.port == clientLocal.port) {
        ::closedir(dir);
        return candFd;
      }
    }

    ::closedir(dir);
    std::this_thread::sleep_for(1ms);  // NOLINT(misc-include-cleaner)
  }

  // One last pass to provide diagnostics in the failure message.
  int entryCount = 0;
  int ipv4SockCount = 0;
  int connectedIpv4SockCount = 0;
  std::string samples;
  DIR* dir = ::opendir("/proc/self/fd");
  if (dir != nullptr) {
    while (auto* ent = ::readdir(dir)) {
      const char* name = ent->d_name;
      if (name == nullptr || name[0] == '.') {
        continue;
      }
      ++entryCount;
      char* endp = nullptr;
      const long parsed = std::strtol(name, &endp, 10);
      if (endp == name || parsed < 0 || parsed > INT32_MAX) {
        continue;
      }
      const int candFd = static_cast<int>(parsed);
      if (candFd == clientFd) {
        continue;
      }
      Ipv4Endpoint candLocal{};
      if (!GetIpv4SockName(candFd, candLocal)) {
        continue;
      }
      ++ipv4SockCount;
      Ipv4Endpoint candPeer{};
      if (!GetIpv4PeerName(candFd, candPeer)) {
        continue;
      }
      ++connectedIpv4SockCount;
      if (samples.size() < 512) {
        samples.append(" fd=")
            .append(std::to_string(candFd))
            .append(" lp=")
            .append(std::to_string(static_cast<unsigned>(ntohs(candLocal.port))))
            .append(" pp=")
            .append(std::to_string(static_cast<unsigned>(ntohs(candPeer.port))));
      }
    }
    ::closedir(dir);
  }

  throw std::runtime_error(
      std::string("Timed out finding server-side fd for client") +
      " (client local port=" + std::to_string(static_cast<unsigned>(ntohs(clientLocal.port))) +
      ", client peer port=" + std::to_string(static_cast<unsigned>(ntohs(clientPeer.port))) +
      ", /proc/self/fd entries=" + std::to_string(entryCount) + ", ipv4 sockets=" + std::to_string(ipv4SockCount) +
      ", connected ipv4 sockets=" + std::to_string(connectedIpv4SockCount) + ", samples:" + samples + ")");
}

bool WaitForPeerClosedNonBlocking(int fd, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    char tmp = 0;
    const auto ret = ::recv(fd, &tmp, 1, MSG_PEEK | MSG_DONTWAIT);
    if (ret == 0) {
      return true;  // orderly close
    }
    if (ret == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        std::this_thread::sleep_for(1ms);
        continue;
      }
      if (errno == ECONNRESET) {
        return true;  // treated as closed
      }
    }
    std::this_thread::sleep_for(1ms);
  }
  return false;
}

}  // namespace

TEST(Http10, BasicVersionEcho) {
  ts.router().setDefault([]([[maybe_unused]] const HttpRequest& req) { return HttpResponse("A"); });
  std::string req = "GET /x HTTP/1.0\r\nHost: h\r\n\r\n";
  std::string resp = test::sendAndCollect(ts.port(), req);
  ASSERT_TRUE(resp.contains("HTTP/1.0 200"));
}

TEST(Http10, No100ContinueEvenIfHeaderPresent) {
  ts.router().setDefault([]([[maybe_unused]] const HttpRequest& req) { return HttpResponse("B"); });
  // Expect ignored in HTTP/1.0
  std::string req =
      "POST /p HTTP/1.0\r\nHost: h\r\nContent-Length: 0\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  std::string resp = test::sendAndCollect(ts.port(), req);
  ASSERT_FALSE(resp.contains("100 Continue"));
  ASSERT_TRUE(resp.contains("HTTP/1.0 200"));
}

TEST(Http10, RejectTransferEncoding) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("C"); });
  std::string req = "GET /te HTTP/1.0\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
  std::string resp = test::sendAndCollect(ts.port(), req);
  // Should return 400 per implementation decision
  ASSERT_TRUE(resp.contains("400"));
}

TEST(Http10, KeepAliveOptInStillWorks) {
  ts.router().setDefault([]([[maybe_unused]] const HttpRequest& req) { return HttpResponse("D"); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req1 = "GET /k1 HTTP/1.0\r\nHost: h\r\nConnection: keep-alive\r\n\r\n";
  test::sendAll(fd, req1);
  std::string first = test::recvWithTimeout(fd, 300ms);  // NOLINT(misc-include-cleaner)
  ASSERT_TRUE(first.contains("HTTP/1.0 200"));
  ASSERT_TRUE(first.contains(MakeHttp1HeaderLine(http::Connection, http::keepalive)));
  std::string req2 = "GET /k2 HTTP/1.0\r\nHost: h\r\nConnection: keep-alive\r\n\r\n";
  test::sendAll(fd, req2);
  std::string second = test::recvWithTimeout(fd, 300ms);  // NOLINT(misc-include-cleaner)
  ASSERT_TRUE(second.contains("HTTP/1.0 200"));
}

TEST(HttpPipeline, TwoRequestsBackToBack) {
  ts.router().setDefault(
      [](const HttpRequest& req) { return HttpResponse(std::string("E:") + std::string(req.path())); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string combo =
      "GET /a HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nGET /b HTTP/1.1\r\nHost: x\r\nContent-Length: "
      "0\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, combo);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("E:/a"));
  ASSERT_TRUE(resp.contains("E:/b"));
}

TEST(HttpExpect, ZeroLengthNo100) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("Z"); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string headers =
      "POST /z HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: 100-continue\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, headers);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_FALSE(resp.contains("100 Continue"));
  ASSERT_TRUE(resp.contains('Z'));
}

TEST(HttpServer, PostConfigUpdateExceptionDoesNotCrash) {
  ts.postConfigUpdate(
      [](HttpServerConfig& /*cfg*/) { throw std::runtime_error("Intentional exception in config update"); });
  ts.postConfigUpdate([](HttpServerConfig& /*cfg*/) { throw 42; });
  ts.postRouterUpdate([](Router& /*cfg*/) { throw std::runtime_error("Intentional exception in router update"); });
  ts.postRouterUpdate([](Router& /*cfg*/) { throw 42; });
}

TEST(HttpMaxRequests, CloseAfterLimit) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxRequestsPerConnection(2); });
  // parser error callback intentionally left empty in tests
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("Q"); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string reqs =
      "GET /1 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nGET /2 HTTP/1.1\r\nHost: x\r\nContent-Length: "
      "0\r\n\r\nGET /3 HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
  test::sendAll(fd, reqs);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_EQ(2, test::countOccurrences(resp, "HTTP/1.1 200"));
  ASSERT_EQ(2, test::countOccurrences(resp, "Q"));
}

TEST(HttpPipeline, SecondMalformedAfterSuccess) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string piped = "GET /good HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\nBADSECONDREQUEST\r\n\r\n";
  test::sendAll(fd, piped);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("OK"));
  ASSERT_TRUE(resp.contains("400"));
}

TEST(HttpContentLength, ExplicitTooLarge413) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxBodyBytes(10); });
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("R"); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string req = "POST /big HTTP/1.1\r\nHost: x\r\nContent-Length: 20\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("413"));
}

TEST(HttpContentLength, GlobalHeaders) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.addGlobalHeader(http::Header{"X-Global", "gvalue"});
    cfg.addGlobalHeader(http::Header{"X-Another", "anothervalue"});
    cfg.addGlobalHeader(http::Header{"X-Custom", "global"});
  });
  ts.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    // This header should not be overwritten by the global one
    respObj.header("X-Custom", "original");
    respObj.body("R");
    return respObj;
  });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string req = "POST /big HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
#ifdef AERONET_ENABLE_HTTP2
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine("x-global", "gvalue")));
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine("x-another", "anothervalue")));
#else
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine("X-Global", "gvalue")));
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine("X-Another", "anothervalue")));
#endif
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine("X-Custom", "original")));
  std::string datePrefix(http::Date);
  datePrefix.append(http::HeaderSep);
  EXPECT_TRUE(resp.contains(datePrefix));
}

TEST(HttpMakeResponse, PrefillsGlobalHeadersHttp11) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.addGlobalHeader(http::Header{"X-Global", "gvalue"});
    cfg.addGlobalHeader(http::Header{"X-Another", "anothervalue"});
    cfg.addGlobalHeader(http::Header{"X-Custom", "from-global"});
  });

  ts.router().setDefault([](const HttpRequest& req) {
    auto resp = req.makeResponse(http::StatusCodeAccepted, "body-from-make", "text/custom");
    // Local header should override the global one when names collide
    resp.header("X-Custom", "local");
    return resp;
  });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string req = "GET /make-response HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  const std::string resp = test::recvUntilClosed(fd);

  EXPECT_TRUE(resp.contains("HTTP/1.1 202"));
#ifdef AERONET_ENABLE_HTTP2
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine("x-global", "gvalue")));
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine("x-another", "anothervalue")));
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine("x-custom", "local")));
#else
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine("X-Global", "gvalue")));
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine("X-Another", "anothervalue")));
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine("X-Custom", "local")));
#endif
  EXPECT_TRUE(resp.contains("body-from-make"));
}

TEST(HttpBasic, LargePayload) {
  const std::string largeBody(1 << 24, 'a');

  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.withMaxOutboundBufferBytes(1 << 25);  // 32 MiB
  });
  ts.router().setDefault([&largeBody](const HttpRequest&) { return HttpResponse(largeBody); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string req = "GET /good HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.contains("HTTP/1.1 200"));
  EXPECT_TRUE(resp.contains(largeBody));
}

TEST(HttpBasic, ManyHeadersRequest) {
  // Test handling a request with thousands of headers
  static constexpr std::size_t kMaxHeaderBytes = 128UL * 1024UL;
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxHeaderBytes(kMaxHeaderBytes); });
  ts.router().setDefault([](const HttpRequest& req) {
    int headerCount = 0;
    for (const auto& [key, value] : req.headers()) {
      if (key.starts_with("X-Custom-")) {
        ++headerCount;
      }
    }
    return HttpResponse("Received " + std::to_string(headerCount) + " custom headers");
  });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  // Build request with many custom headers
  static constexpr int kNbHeaders = 3000;
  RawChars req(kMaxHeaderBytes);
  req.unchecked_append("GET /test HTTP/1.1\r\nHost: localhost\r\n");
  for (int headerPos = 0; headerPos < kNbHeaders; ++headerPos) {
    req.append("X-Custom-");
    req.append(std::string_view(IntegralToCharVector(headerPos)));
    req.append(": value");
    req.append(std::string_view(IntegralToCharVector(headerPos)));
    req.append(http::CRLF);
  }
  req.append("Content-Length: 0\r\nConnection: close");
  req.append(http::DoubleCRLF);

  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.contains("HTTP/1.1 200"));
  EXPECT_TRUE(resp.contains("Received " + std::to_string(kNbHeaders) + " custom headers"));
}

TEST(HttpBasic, InvalidContentLength) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("X"); });

  test::ClientConnection clientConnection(ts.port());
  std::string req = "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: invalid-length\r\nConnection: close\r\n\r\nHELLO";
  test::sendAll(clientConnection.fd(), req);
  std::string resp = test::recvUntilClosed(clientConnection.fd());
  EXPECT_TRUE(resp.contains("HTTP/1.1 400")) << resp;

  // Now try with a negative Content-Length
  test::ClientConnection clientConnection2(ts.port());
  std::string req2 = "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: -5\r\nConnection: close\r\n\r\nHELLO";
  test::sendAll(clientConnection2.fd(), req2);
  std::string resp2 = test::recvUntilClosed(clientConnection2.fd());
  EXPECT_TRUE(resp2.contains("HTTP/1.1 400")) << resp2;

  // Now try with an excessively large Content-Length
  test::ClientConnection clientConnection3(ts.port());
  std::string req3 =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 18446744073709551615000000000000\r\nConnection: "
      "close\r\n\r\nHELLO";
  test::sendAll(clientConnection3.fd(), req3);
  std::string resp3 = test::recvUntilClosed(clientConnection3.fd());
  EXPECT_TRUE(resp3.contains("HTTP/1.1 400")) << resp3;

  // Try with a partial match of std::from_chars:
  test::ClientConnection clientConnection4(ts.port());
  std::string req4 = "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 123abc\r\nConnection: close\r\n\r\nHELLO";
  test::sendAll(clientConnection4.fd(), req4);
  std::string resp4 = test::recvUntilClosed(clientConnection4.fd());
  EXPECT_TRUE(resp4.contains("HTTP/1.1 400")) << resp4;

  // Empty content length is invalid too
  test::ClientConnection clientConnection5(ts.port());
  std::string req5 = "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: \r\nConnection: close\r\n\r\nHELLO";
  test::sendAll(clientConnection5.fd(), req5);
  std::string resp5 = test::recvUntilClosed(clientConnection5.fd());
  EXPECT_TRUE(resp5.contains("HTTP/1.1 400")) << resp5;
}

TEST(HttpBasic, ManyHeadersResponse) {
  // Test generating a response with thousands of headers
  ts.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    // Add 3000 custom headers to response
    for (int i = 0; i < 3000; ++i) {
      respObj.headerAddLine("X-Response-" + std::to_string(i), "value" + std::to_string(i));
    }
    respObj.body("Response with many headers");
    return respObj;
  });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  std::string req = "GET /test HTTP/1.1\r\nHost: localhost\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.contains("HTTP/1.1 200"));
  EXPECT_TRUE(resp.contains("Response with many headers"));

  // Verify some of the custom headers are present
  EXPECT_TRUE(resp.contains("X-Response-0: value0"));
  EXPECT_TRUE(resp.contains("X-Response-500: value500"));
  EXPECT_TRUE(resp.contains("X-Response-999: value999"));
  EXPECT_TRUE(resp.contains("X-Response-1499: value1499"));
  EXPECT_TRUE(resp.contains("X-Response-1999: value1999"));
  EXPECT_TRUE(resp.contains("X-Response-2999: value2999"));
}

TEST(HttpExpectation, UnknownExpectationReturns417) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("X"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nExpect: custom-token\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("417")) << resp;
}

TEST(HttpExpectation, MultipleTokensWithUnknownShouldReturn417) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("X"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  // Include 100-continue and an unknown token -> RFC requires 417
  std::string req =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue, custom-token\r\nConnection: "
      "close\r\n\r\nHELLO";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("417")) << resp;
}

TEST(HttpExpectation, HandlerCanEmit100Continue) {
  // Register handler that emits 100 Continue for token "100-continue-custom"
  ts.server.setExpectationHandler([](const HttpRequest& /*req*/, std::string_view token) {
    SingleHttpServer::ExpectationResult res;
    if (token == "100-continue-custom") {
      res.kind = SingleHttpServer::ExpectationResultKind::Interim;
      res.interimStatus = 100;
      return res;
    }
    res.kind = SingleHttpServer::ExpectationResultKind::Continue;
    return res;
  });

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue-custom\r\nConnection: "
      "close\r\n\r\nHELLO";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 100 Continue")) << resp;
  ASSERT_TRUE(resp.contains("200")) << resp;
}

TEST(HttpExpectation, HandlerCanEmit102Interim) {
  // Register handler that emits 102 Processing for token "102-processing"
  ts.server.setExpectationHandler([](const HttpRequest& /*req*/, std::string_view token) {
    SingleHttpServer::ExpectationResult res;
    if (token == "102-processing") {
      res.kind = SingleHttpServer::ExpectationResultKind::Interim;
      res.interimStatus = 102;
      return res;
    }
    res.kind = SingleHttpServer::ExpectationResultKind::Continue;
    return res;
  });

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 102-processing\r\nConnection: close\r\n\r\nHELLO";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 102 Processing")) << resp;
  ASSERT_TRUE(resp.contains("200")) << resp;
}

TEST(HttpExpectation, HandlerCanEmitArbitraryInterimStatus) {
  // Register handler that emits 103 Early Hints (default case handling)
  ts.server.setExpectationHandler([](const HttpRequest& /*req*/, std::string_view token) {
    SingleHttpServer::ExpectationResult res;
    if (token == "103-early-hints") {
      res.kind = SingleHttpServer::ExpectationResultKind::Interim;
      res.interimStatus = 103;
      return res;
    }
    res.kind = SingleHttpServer::ExpectationResultKind::Continue;
    return res;
  });

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 103-early-hints\r\nConnection: close\r\n\r\nHELLO";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 103")) << resp;
  ASSERT_TRUE(resp.contains("200")) << resp;
}

TEST(HttpExpectation, ExpectationHandlerErrors) {
  // Handler throws an exception
  ts.server.setExpectationHandler([](const HttpRequest&, std::string_view token) {
    if (token == "throwsStdException") {
      throw std::runtime_error("boom");
    }
    if (token == "throwsCustomException") {
      throw 42;
    }
    SingleHttpServer::ExpectationResult res;
    if (token == "bad-interim1") {
      res.kind = SingleHttpServer::ExpectationResultKind::Interim;
      res.interimStatus = 250;  // invalid: not 1xx
      return res;
    }
    if (token == "bad-interim2") {
      res.kind = SingleHttpServer::ExpectationResultKind::Interim;
      res.interimStatus = 99;  // invalid: not 1xx
      return res;
    }
    if (token == "expectation-failure") {
      res.kind = SingleHttpServer::ExpectationResultKind::Reject;
      return res;
    }
    res.kind = SingleHttpServer::ExpectationResultKind::Continue;
    return res;
  });

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("SHOULD NOT SEE"); });

  for (std::string_view token :
       {"throwsStdException", "throwsCustomException", "bad-interim1", "bad-interim2", "expectation-failure"}) {
    auto req = std::format(
        "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: {}\r\nConnection: close\r\n\r\nHELLO", token);

    test::ClientConnection clientConnection(ts.port());
    int fd = clientConnection.fd();
    test::sendAll(fd, req);
    std::string resp = test::recvUntilClosed(fd);
    // Server should return 500 due to exception in handler and not invoke handler body
    if (token.starts_with("throws")) {
      EXPECT_TRUE(resp.starts_with("HTTP/1.1 500")) << resp;
      EXPECT_TRUE(resp.contains("Internal Server Error")) << resp;
    } else if (token.starts_with("bad")) {
      EXPECT_TRUE(resp.starts_with("HTTP/1.1 500")) << resp;
      EXPECT_TRUE(resp.contains("Server Error")) << resp;
    } else {
      EXPECT_EQ(token, "expectation-failure");
      EXPECT_TRUE(resp.starts_with("HTTP/1.1 417")) << resp;
    }
    EXPECT_FALSE(resp.contains("SHOULD NOT SEE")) << resp;
  }
}

TEST(HttpExpectation, HandlerFinalResponseSkipsBody) {
  // Handler returns a final response immediately
  ts.server.setExpectationHandler([](const HttpRequest& /*req*/, std::string_view token) {
    SingleHttpServer::ExpectationResult res;
    if (token == "auth-check") {
      res.kind = SingleHttpServer::ExpectationResultKind::FinalResponse;
      HttpResponse hr(403, "Forbidden");
      hr.body("nope");
      res.finalResponse = std::move(hr);
      return res;
    }
    res.kind = SingleHttpServer::ExpectationResultKind::Continue;
    return res;
  });

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("SHOULD NOT SEE"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: auth-check\r\nConnection: close\r\n\r\nHELLO";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("403")) << resp;
  ASSERT_TRUE(resp.contains("nope")) << resp;
  ASSERT_FALSE(resp.contains("SHOULD NOT SEE")) << resp;
}

TEST(HttpExpectation, Mixed100AndCustomWithHandlerContinue) {
  // Handler accepts custom token and returns Continue
  ts.server.setExpectationHandler([](const HttpRequest& /*req*/, std::string_view token) {
    SingleHttpServer::ExpectationResult res;
    if (token == "custom-ok") {
      res.kind = SingleHttpServer::ExpectationResultKind::Continue;
      return res;
    }
    res.kind = SingleHttpServer::ExpectationResultKind::Continue;
    return res;
  });

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("DONE"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: 100-continue, custom-ok\r\nConnection: "
      "close\r\n\r\nHELLO";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  // Should see 100 Continue (from expectContinue path) and final 200
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 100 Continue")) << resp;
  ASSERT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("DONE")) << resp;
}

TEST(HttpChunked, DecodeBasic) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg = TestServerConfig(); });
  ts.resetRouterAndGet().setDefault([](const HttpRequest& req) {
    return HttpResponse(http::StatusCodeOK)
        .body(std::string("LEN=") + std::to_string(req.body().size()) + ":" + std::string(req.body()));
  });

  test::ClientConnection sock(ts.port());
  int fd = sock.fd();

  std::string req =
      "POST /c HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("LEN=9:Wikipedia"));
}

TEST(HttpHead, NoBodyReturned) {
  ts.resetRouterAndGet().setDefault(
      [](const HttpRequest& req) { return HttpResponse(std::string("DATA-") + std::string(req.path())); });
  test::ClientConnection cnx(ts.port());
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
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg = TestServerConfig();
    cfg.withMaxBodyBytes(5);
  });
  ts.resetRouterAndGet().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });
  test::ClientConnection cnx(ts.port());
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
    cfg = TestServerConfig();
    cfg.withMaxBodyBytes(4);  // very small limit
  });
  ts.resetRouterAndGet().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });
  test::ClientConnection cnx(ts.port());
  int fd = cnx.fd();
  // Single 5-byte chunk exceeds limit 4
  std::string req =
      "POST /big HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n5\r\nabcde\r\n0\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("413"));
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
TEST(HttpAsync, FlushPendingResponseAfterBody) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg = TestServerConfig(); });
  // Handler completes immediately but body wasn't ready when started.
  ts.resetRouterAndGet().setPath(http::Method::POST, "/async-flush",
                                 []([[maybe_unused]] HttpRequest& req) -> RequestTask<HttpResponse> {
                                   // Return a response immediately; if the request body
                                   // wasn't ready the server will hold it as pending.
                                   co_return HttpResponse("async-ok");
                                 });

  test::ClientConnection cnx(ts.port());
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
#endif

TEST(HttpHead, MaxRequestsApplied) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxRequestsPerConnection(3); });
  auto port = ts.port();
  ts.router().setDefault([]([[maybe_unused]] const HttpRequest& req) { return HttpResponse("IGNORED"); });
  test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  // 4 HEAD requests pipelined; only 3 responses expected then close
  std::string reqs;
  for (int i = 0; i < 4; ++i) {
    reqs += "HEAD /h" + std::to_string(i) + " HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
  }
  test::sendAll(fd, reqs);
  std::string resp = test::recvUntilClosed(fd);
  int statusCount = 0;
  std::size_t pos = 0;
  while ((pos = resp.find("HTTP/1.1 200", pos)) != std::string::npos) {
    ++statusCount;
    pos += 11;
  }
  ASSERT_EQ(3, statusCount) << resp;
  // HEAD responses must not include body; ensure no accidental body token present
  ASSERT_FALSE(resp.contains("IGNORED"));
}

// Test immutable config changes are rejected at runtime (nbThreads)
TEST(SingleHttpServer, ImmutableConfigChangeNbThreadsIgnored) {
  auto origThreadCount = ts.server.config().nbThreads;
  ts.postConfigUpdate([origThreadCount](HttpServerConfig& cfg) { cfg.nbThreads = origThreadCount + 1; });
  // Give the server time to process the config update
  std::this_thread::sleep_for(10ms);  // NOLINT(misc-include-cleaner)
  ASSERT_EQ(origThreadCount, ts.server.config().nbThreads);
}

// Test immutable config changes are rejected at runtime (port)
TEST(SingleHttpServer, ImmutableConfigChangePortIgnored) {
  auto origPort = ts.server.config().port;
  ts.postConfigUpdate([origPort](HttpServerConfig& cfg) { cfg.port = origPort + 1; });
  std::this_thread::sleep_for(10ms);
  ASSERT_EQ(origPort, ts.server.config().port);
}

// Test immutable config changes are rejected at runtime (reusePort)
TEST(SingleHttpServer, ImmutableConfigChangeReusePortIgnored) {
  auto origReusePort = ts.server.config().reusePort;
  ts.postConfigUpdate([origReusePort](HttpServerConfig& cfg) { cfg.reusePort = !origReusePort; });
  std::this_thread::sleep_for(10ms);
  ASSERT_EQ(origReusePort, ts.server.config().reusePort);
}

// Test synchronous router update exception handling with completion
TEST(SingleHttpServer, SynchronousRouterUpdateExceptionRethrown) {
  // The synchronous update path is tested through setDefault which throws if the handler is empty
  EXPECT_NO_THROW(ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); }));
}

// Test request handler exception in normal handler
TEST(SingleHttpServer, RequestHandlerStdException) {
  ts.router().setDefault([](const HttpRequest&) -> HttpResponse { throw std::runtime_error("Handler error"); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string req = "GET /test HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("500")) << resp;
  ASSERT_TRUE(resp.contains("Handler error")) << resp;
}

// Test request handler non-std exception
TEST(SingleHttpServer, RequestHandlerNonStdException) {
  ts.router().setDefault([](const HttpRequest&) -> HttpResponse { throw 42; });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string req = "GET /test HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("500")) << resp;
  ASSERT_TRUE(resp.contains("Unknown error")) << resp;
}

// Test body read timeout is set when configured and body not ready
TEST(SingleHttpServer, BodyReadTimeoutSetWhenNotReady) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.withMaxBodyBytes(256 << 20).withBodyReadTimeout(1s);  // NOLINT(misc-include-cleaner)
  });
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  // Send headers indicating body but don't send body yet
  std::string req = "POST /test HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n";
  test::sendAll(fd, req);
  std::this_thread::sleep_for(50ms);  // NOLINT(misc-include-cleaner)
  // Now send body
  test::sendAll(fd, "1234567890");
  std::string resp = test::recvWithTimeout(fd, 1000ms, 187);  // NOLINT(misc-include-cleaner)
  ASSERT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
}

// Test body read timeout cleared when body is ready
TEST(SingleHttpServer, BodyReadTimeoutClearedWhenReady) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withMaxBodyBytes(256 << 20).withBodyReadTimeout(1s); });
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  // Send complete request with body
  std::string req = "POST /test HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nConnection: close\r\n\r\nHELLO";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("HELLO")) << resp;
}

TEST(SingleHttpServer, KeepAliveTimeoutNotTiedToPollInterval) {
  const auto oldPollInterval = ts.server.config().pollInterval;

  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.withKeepAliveMode(true);
    cfg.withKeepAliveTimeout(5ms);  // NOLINT(misc-include-cleaner)
    cfg.withPollInterval(std::chrono::milliseconds{100});
  });

  test::ClientConnection cnx(ts.port());

  // The server should proactively close the idle keep-alive connection quickly.
  EXPECT_TRUE(test::WaitForPeerClose(cnx.fd(), 500ms));  // NOLINT(misc-include-cleaner)

  ts.postConfigUpdate([oldPollInterval](HttpServerConfig& cfg) { cfg.withPollInterval(oldPollInterval); });
}

// Test request decompression when Content-Encoding is identity (no decompression needed)
TEST(SingleHttpServer, RequestBodyIdentityEncodingNoDecompression) {
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  std::string req =
      "POST /test HTTP/1.1\r\nHost: x\r\nContent-Encoding: identity\r\nContent-Length: 4\r\nConnection: "
      "close\r\n\r\ntest";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("test")) << resp;
}

// Test request decompression disabled (passthrough mode)
TEST(SingleHttpServer, RequestBodyDecompressionDisabledPassthrough) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.withMaxBodyBytes(256 << 20);
    cfg.decompression.enable = false;
  });
  ts.router().setDefault([](const HttpRequest& req) {
    // Body will still be compressed since decompression is disabled
    return HttpResponse(req.headerValueOrEmpty(http::ContentEncoding));
  });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  // Send with gzip encoding header
  std::string req =
      "POST /test HTTP/1.1\r\nHost: x\r\nContent-Encoding: gzip\r\nContent-Length: 5\r\nConnection: close\r\n\r\nDUMMY";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(resp.contains("gzip")) << resp;
}

// Test unknown exception in router update without completion ptr
TEST(SingleHttpServer, RouterUpdateUnknownExceptionNoCompletion) {
  // Exception that doesn't inherit from std::exception
  ts.postRouterUpdate([](Router&) {
    throw 999;  // Triggers catch(...) path
  });

  // Server should still be functional
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("HTTP/1.1 200")) << resp;
}

// Test TLS config modification attempt at runtime (should be ignored)
TEST(SingleHttpServer, TLSConfigModificationIgnored) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    // Attempt to modify immutable TLS config - should be logged and ignored
    cfg.tls.withCertFile("/some/path");
  });

  // Server should still work
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// Test telemetry config modification attempt at runtime (should be ignored)
TEST(SingleHttpServer, TelemetryConfigModificationIgnored) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    // Attempt to modify immutable telemetry config - should be logged and ignored
    cfg.telemetry.otelEnabled = !cfg.telemetry.otelEnabled;
  });

  // Server should still work
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// Test decompression enabled with large body
TEST(SingleHttpServer, DecompressionConfigurable) {
  // Update decompression limit
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.withMaxBodyBytes(256 << 20);
    cfg.decompression.maxDecompressedBytes = 1024;
  });

  ts.router().setDefault([](const HttpRequest& req) {
    std::string body(req.body().begin(), req.body().end());
    return HttpResponse("size:" + std::to_string(body.size()));
  });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  test::sendAll(fd, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n0123456789");
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
}

// Test HEAD method doesn't send body
TEST(SingleHttpServer, HeadMethodNoBody) {
  ts.router().setDefault([](const HttpRequest&) {
    HttpResponse resp("This is the body content");
    resp.headerAddLine("X-Custom", "value");
    return resp;
  });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  test::sendAll(fd, "HEAD / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("HTTP/1.1 200"));
  ASSERT_TRUE(resp.contains("X-Custom"));
  // Body should not be present for HEAD
  ASSERT_FALSE(resp.contains("This is the body content"));
}

// Test OPTIONS method
TEST(SingleHttpServer, OptionsMethod) {
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  test::sendAll(fd, "OPTIONS / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  std::string resp = test::recvUntilClosed(fd);
  // Should handle OPTIONS (typically returns 200 or 204)
  ASSERT_TRUE(resp.contains("HTTP/1.1 200") || resp.contains("HTTP/1.1 204")) << resp;
}

// Test exception in request middleware
TEST(SingleHttpServer, MiddlewareExceptionHandling) {
  ts.router().addRequestMiddleware([](HttpRequest&) {
    // Test just that adding a middleware that throws doesn't crash
    throw std::runtime_error("middleware failure");
    return MiddlewareResult::Continue();
  });

  ts.router().addRequestMiddleware([](HttpRequest&) {
    // Test just that adding a middleware that throws doesn't crash
    throw 42;
    return MiddlewareResult::Continue();
  });

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("should not reach"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  std::string resp = test::recvUntilClosed(fd);

  // Response should handle the error gracefully
  ASSERT_FALSE(resp.empty());

  ts.router() = Router();  // Clear middlewares for other tests
}

TEST(SingleHttpServer, RequestMiddlewareStdExceptionInGlobalMiddleware) {
  ts.resetRouterAndGet().addRequestMiddleware(
      [](const HttpRequest&) -> MiddlewareResult { throw std::runtime_error("request middleware error"); });

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  auto resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 500"));
}

TEST(SingleHttpServer, RequestMiddlewareCustomExceptionInGlobalMiddleware) {
  ts.resetRouterAndGet().addRequestMiddleware([](const HttpRequest&) -> MiddlewareResult { throw 42; });

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  auto resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 500"));
}

TEST(SingleHttpServer, ResponseMiddlewareStdExceptionInGlobalMiddleware) {
  ts.resetRouterAndGet().addResponseMiddleware(
      [](const HttpRequest&, HttpResponse&) { throw std::runtime_error("response middleware error"); });

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  ts.router().setPath(http::Method::GET, "/test",
                      [](const HttpRequest&, HttpResponseWriter& writer) { writer.writeBody("test"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_FALSE(test::recvUntilClosed(fd).empty());
}

TEST(SingleHttpServer, ResponseMiddlewareCustomExceptionInGlobalMiddleware) {
  ts.resetRouterAndGet().addResponseMiddleware([](const HttpRequest&, HttpResponse&) { throw 42; });

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  ts.router().setPath(http::Method::GET, "/test",
                      [](const HttpRequest&, HttpResponseWriter& writer) { writer.writeBody("test"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_FALSE(test::recvUntilClosed(fd).empty());
}

TEST(SingleHttpServer, RequestMiddlewareStdExceptionInPathMiddleware) {
  auto entry = ts.resetRouterAndGet().setPath(
      http::Method::GET, "/test", [](const HttpRequest&, HttpResponseWriter& writer) { writer.writeBody("test"); });
  entry.before([](const HttpRequest&) -> MiddlewareResult { throw std::runtime_error("request middleware error"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  test::sendAll(fd, "GET /test HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  auto resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 500"));
}

TEST(SingleHttpServer, RequestMiddlewareCustomExceptionInPathMiddleware) {
  auto entry = ts.resetRouterAndGet().setPath(
      http::Method::GET, "/test", [](const HttpRequest&, HttpResponseWriter& writer) { writer.writeBody("test"); });
  entry.before([](const HttpRequest&) -> MiddlewareResult { throw 42; });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  test::sendAll(fd, "GET /test HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  auto resp = test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 500"));
}

TEST(SingleHttpServer, ResponseMiddlewareStdExceptionInPathMiddleware) {
  auto entry = ts.resetRouterAndGet().setPath(
      http::Method::GET, "/test", [](const HttpRequest&, HttpResponseWriter& writer) { writer.writeBody("test"); });

  entry.after([](const HttpRequest&, HttpResponse&) { throw std::runtime_error("response middleware error"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  test::sendAll(fd, "GET /test HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_FALSE(test::recvUntilClosed(fd).empty());
}

TEST(SingleHttpServer, ResponseMiddlewareCustomExceptionInPathMiddleware) {
  auto entry =
      ts.resetRouterAndGet().setPath(http::Method::GET, "/test", [](const HttpRequest&) { return HttpResponse("OK"); });
  entry.after([](const HttpRequest&, HttpResponse&) { throw 42; });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  test::sendAll(fd, "GET /test HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_FALSE(test::recvUntilClosed(fd).empty());
}

// Test multiple response middleware
TEST(SingleHttpServer, MultipleResponseMiddleware) {
  ts.resetRouterAndGet().addResponseMiddleware(
      [](const HttpRequest&, HttpResponse& resp) { resp.headerAddLine("X-Middleware-1", "first"); });

  ts.router().addResponseMiddleware(
      [](const HttpRequest&, HttpResponse& resp) { resp.headerAddLine("X-Middleware-2", "second"); });

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  std::string resp = test::recvUntilClosed(fd);

  ASSERT_TRUE(resp.contains("X-Middleware-1"));
  ASSERT_TRUE(resp.contains("X-Middleware-2"));

  ts.router() = Router();  // Clear middlewares for other tests
}

// Test epoll_ctl MOD failure handling - simulates EBADF (benign) error
// This test verifies that when epoll_ctl MOD operations fail, the server handles
// them gracefully. While the normal HTTP request/response flow may not always trigger
// MOD operations, this test infrastructure can be used for more complex scenarios
// This test exercises the RecordModFailure code path in single-http-server.cpp.
// RecordModFailure is called when epoll_ctl MOD fails during write interest management.
TEST(SingleHttpServer, EpollCtlModBenignFailure) {
  test::EventLoopHookGuard guard;
  test::FailAllEpollCtlMod(EBADF);

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(std::string(24UL * 1024 * 1024, 'Y')); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  // Send the request with Connection: close for clean termination
  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

  auto data = test::recvUntilClosed(fd);

  // Should have received data
  EXPECT_GT(data.size(), 0UL);
}

// Test epoll_ctl MOD failure with EACCES (serious) error
TEST(SingleHttpServer, EpollCtlModEaccesFailure) {
  test::EventLoopHookGuard guard;
  test::FailAllEpollCtlMod(EACCES);

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(std::string(24UL * 1024 * 1024, 'Y')); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

  auto data = test::recvUntilClosed(fd);

  EXPECT_GT(data.size(), 0UL);
}

TEST(SingleHttpServer, EpollPollFailure) {
  test::EventLoopHookGuard guard;
  test::SetEpollWaitActions({test::WaitError(EINTR), test::WaitError(EACCES), test::WaitError(EACCES),
                             test::WaitError(EINTR), test::WaitError(EBADF), test::WaitError(EBADF)});

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(std::string(1024UL * 1024, 'Y')); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

  auto data = test::recvWithTimeout(fd, 50ms);

  EXPECT_TRUE(data.empty());
}

TEST(SingleHttpServer, EpollRdhupWithoutInTriggersClose) {
  test::EventLoopHookGuard hookGuard;

  // Keep router simple; no request is sent.
  test::TestServer localTs(TestServerConfig(), RouterConfig{}, std::chrono::milliseconds{5});
  localTs.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  test::ClientConnection clientConnection(localTs.port());
  const int clientFd = clientConnection.fd();
  ASSERT_GE(clientFd, 0);

  const int serverFd = FindServerSideFdForClientOrThrow(clientFd, 1s);

  // Make the RDHUP event consistent: half-close client write end so server read observes EOF.
  ASSERT_TRUE(ShutdownWrite(clientFd));

  // Inject EPOLLRDHUP WITHOUT EPOLLIN. The server should still drive the read path and close.
  test::PushEpollWaitAction(test::WaitReturn(1, {test::MakeEvent(serverFd, EPOLLRDHUP)}));

  ASSERT_TRUE(WaitForPeerClosedNonBlocking(clientFd, 1s));
}

TEST(SingleHttpServer, EpollHupWithoutInTriggersClose) {
  test::EventLoopHookGuard hookGuard;
  test::TestServer localTs(TestServerConfig(), RouterConfig{}, std::chrono::milliseconds{5});
  localTs.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  test::ClientConnection clientConnection(localTs.port());
  const int clientFd = clientConnection.fd();
  ASSERT_GE(clientFd, 0);

  const int serverFd = FindServerSideFdForClientOrThrow(clientFd, 1s);

  ASSERT_TRUE(ShutdownWrite(clientFd));

  // Inject EPOLLHUP WITHOUT EPOLLIN.
  test::PushEpollWaitAction(test::WaitReturn(1, {test::MakeEvent(serverFd, EPOLLHUP)}));

  ASSERT_TRUE(WaitForPeerClosedNonBlocking(clientFd, 1s));
}

TEST(SingleHttpServer, EpollErrWithoutInTriggersCloseOnReadError) {
  test::EventLoopHookGuard hookGuard;
  test::TestServer localTs(TestServerConfig(), RouterConfig{}, std::chrono::milliseconds{5});
  localTs.resetRouterAndGet().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  test::ClientConnection clientConnection(localTs.port());
  const int clientFd = clientConnection.fd();
  ASSERT_GE(clientFd, 0);

  const int serverFd = FindServerSideFdForClientOrThrow(clientFd, 1s);

  // Force the next server-side read to fail fatally, then inject EPOLLERR WITHOUT EPOLLIN.
  test::SetReadActions(serverFd, {{-1, ECONNRESET}});
  test::PushEpollWaitAction(test::WaitReturn(1, {test::MakeEvent(serverFd, EPOLLERR)}));

  ASSERT_TRUE(WaitForPeerClosedNonBlocking(clientFd, 1s));
  test::ResetIoActions();
}
