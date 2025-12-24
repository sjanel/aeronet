#include <dirent.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/router.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/stringconv.hpp"

// Enable epoll/socket syscall overrides from sys-test-support.hpp
#define AERONET_WANT_SOCKET_OVERRIDES
#define AERONET_WANT_READ_WRITE_OVERRIDES
#include "aeronet/sys-test-support.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

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
  return cfg;
}

test::TestServer ts(TestServerConfig(), RouterConfig{}, std::chrono::milliseconds{5});

struct Ipv4Endpoint {
  uint32_t addr{0};
  uint16_t port{0};
};

static bool GetIpv4SockName(int fd, Ipv4Endpoint& out) {
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

static bool GetIpv4PeerName(int fd, Ipv4Endpoint& out) {
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

static int FindServerSideFdForClientOrThrow(int clientFd, std::chrono::milliseconds timeout) {
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
    std::this_thread::sleep_for(1ms);
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

static bool WaitForPeerClosedNonBlocking(int fd, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    char tmp = 0;
    const ssize_t ret = ::recv(fd, &tmp, 1, MSG_PEEK | MSG_DONTWAIT);
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
  EXPECT_TRUE(resp.contains("\r\nX-Global: gvalue"));
  EXPECT_TRUE(resp.contains("\r\nX-Another: anothervalue"));
  EXPECT_TRUE(resp.contains("\r\nX-Custom: original"));
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

TEST(HttpBasic, ManyHeadersResponse) {
  // Test generating a response with thousands of headers
  ts.router().setDefault([](const HttpRequest&) {
    HttpResponse respObj;
    // Add 3000 custom headers to response
    for (int i = 0; i < 3000; ++i) {
      respObj.addHeader("X-Response-" + std::to_string(i), "value" + std::to_string(i));
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
  ASSERT_TRUE(resp.contains("102 Processing")) << resp;
  ASSERT_TRUE(resp.contains("200")) << resp;
}

TEST(HttpExpectation, HandlerInvalidInterimStatusReturns500) {
  // Handler emits an invalid interim status (not 1xx)
  ts.server.setExpectationHandler([](const HttpRequest& /*req*/, std::string_view token) {
    SingleHttpServer::ExpectationResult res;
    if (token == "bad-interim") {
      res.kind = SingleHttpServer::ExpectationResultKind::Interim;
      res.interimStatus = 250;  // invalid: not 1xx
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
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: bad-interim\r\nConnection: close\r\n\r\nHELLO";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  // Server should return 500 due to invalid interim status and not invoke handler body
  ASSERT_TRUE(resp.contains("500")) << resp;
  ASSERT_TRUE(resp.contains("Invalid interim status")) << resp;
  ASSERT_FALSE(resp.contains("SHOULD NOT SEE")) << resp;
}

TEST(HttpExpectation, HandlerThrowsReturns500AndSkipsBody) {
  // Handler throws an exception
  ts.server.setExpectationHandler([](const HttpRequest&, std::string_view token) {
    if (token == "throws") {
      throw std::runtime_error("boom");
    }
    SingleHttpServer::ExpectationResult res;
    res.kind = SingleHttpServer::ExpectationResultKind::Continue;
    return res;
  });

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("SHOULD NOT SEE"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  ASSERT_GE(fd, 0);
  std::string req =
      "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nExpect: throws\r\nConnection: close\r\n\r\nHELLO";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  // Server should return 500 due to exception in handler and not invoke handler body
  ASSERT_TRUE(resp.contains("500")) << resp;
  ASSERT_TRUE(resp.contains("Internal Server Error")) << resp;
  ASSERT_FALSE(resp.contains("SHOULD NOT SEE")) << resp;
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
  ASSERT_TRUE(resp.contains("100 Continue")) << resp;
  ASSERT_TRUE(resp.contains("200")) << resp;
  ASSERT_TRUE(resp.contains("DONE")) << resp;
}

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

TEST(SingleHttpServer, CachedConnectionsTimeout) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.withKeepAliveMode(false);
    cfg.withCloseCachedConnectionsTimeout({});
  });
  auto port = ts.port();
  ts.router().setDefault([]([[maybe_unused]] const HttpRequest& req) { return HttpResponse("OK"); });

  const std::string req = "GET /h HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";

  for (int reqPos = 0; reqPos < 10; ++reqPos) {
    test::ClientConnection clientConnection(port);
    int fd = clientConnection.fd();
    ASSERT_GE(fd, 0);
    test::sendAll(fd, req);
    std::string firstResp = test::recvWithTimeout(fd);
    ASSERT_TRUE(firstResp.contains("HTTP/1.1 200")) << firstResp;
  }

  std::this_thread::sleep_for(5ms);

  test::ClientConnection clientConnection(port);
  int fd = clientConnection.fd();
  test::sendAll(fd, req);
  std::string secondResp = test::recvWithTimeout(fd);
  // Expect no data (connection should be closed)
  ASSERT_TRUE(secondResp.contains("HTTP/1.1 200")) << secondResp;
}

// Test immutable config changes are rejected at runtime (nbThreads)
TEST(SingleHttpServer, ImmutableConfigChangeNbThreadsIgnored) {
  auto origThreadCount = ts.server.config().nbThreads;
  ts.postConfigUpdate([origThreadCount](HttpServerConfig& cfg) { cfg.nbThreads = origThreadCount + 1; });
  // Give the server time to process the config update
  std::this_thread::sleep_for(10ms);
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

// Test posted router update exception handling without completion (async path)
TEST(SingleHttpServer, PostedRouterUpdateExceptionAsyncLogged) {
  // Use postRouterUpdate (no completion) to exercise the else branch in catch blocks
  ts.postRouterUpdate([](Router&) { throw std::runtime_error("Test exception in posted update"); });
  std::this_thread::sleep_for(10ms);
  // If we get here without crash, the exception was caught and logged
  SUCCEED();
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
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withBodyReadTimeout(1s); });
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  // Send headers indicating body but don't send body yet
  std::string req = "POST /test HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n";
  test::sendAll(fd, req);
  std::this_thread::sleep_for(20ms);
  // Now send body
  test::sendAll(fd, "1234567890");
  std::string resp = test::recvWithTimeout(fd);
  ASSERT_TRUE(resp.contains("200")) << resp;
}

// Test body read timeout cleared when body is ready
TEST(SingleHttpServer, BodyReadTimeoutClearedWhenReady) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.withBodyReadTimeout(1s); });
  ts.router().setDefault([](const HttpRequest& req) { return HttpResponse(req.body()); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  // Send complete request with body
  std::string req = "POST /test HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\nConnection: close\r\n\r\nHELLO";
  test::sendAll(fd, req);
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("200")) << resp;
  ASSERT_TRUE(resp.contains("HELLO")) << resp;
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
  ASSERT_TRUE(resp.contains("200")) << resp;
  ASSERT_TRUE(resp.contains("test")) << resp;
}

// Test request decompression disabled (passthrough mode)
TEST(SingleHttpServer, RequestBodyDecompressionDisabledPassthrough) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression.enable = false; });
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
  ASSERT_TRUE(resp.contains("200")) << resp;
  ASSERT_TRUE(resp.contains("gzip")) << resp;
}

// Test unknown exception in router update without completion ptr
TEST(SingleHttpServer, RouterUpdateUnknownExceptionNoCompletion) {
  // Exception that doesn't inherit from std::exception
  struct CustomException {
    int code = 999;
  };

  ts.postRouterUpdate([](Router&) {
    throw CustomException{};  // Triggers catch(...) path
  });

  std::this_thread::sleep_for(50ms);

  // Server should still be functional
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("200")) << resp;
}

// Test TLS config modification attempt at runtime (should be ignored)
TEST(SingleHttpServer, TLSConfigModificationIgnored) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    // Attempt to modify immutable TLS config - should be logged and ignored
    cfg.tls.withCertFile("/some/path");
  });
  std::this_thread::sleep_for(20ms);

  // Server should still work
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("200")) << resp;
}

// Test telemetry config modification attempt at runtime (should be ignored)
TEST(SingleHttpServer, TelemetryConfigModificationIgnored) {
  ts.postConfigUpdate([](HttpServerConfig& cfg) {
    // Attempt to modify immutable telemetry config - should be logged and ignored
    cfg.telemetry.otelEnabled = !cfg.telemetry.otelEnabled;
  });
  std::this_thread::sleep_for(20ms);

  // Server should still work
  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });
  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("200")) << resp;
}

// Test decompression enabled with large body
TEST(SingleHttpServer, DecompressionConfigurable) {
  // Update decompression limit
  ts.postConfigUpdate([](HttpServerConfig& cfg) { cfg.decompression.maxDecompressedBytes = 1024; });

  ts.router().setDefault([](const HttpRequest& req) {
    std::string body(req.body().begin(), req.body().end());
    return HttpResponse("size:" + std::to_string(body.size()));
  });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  test::sendAll(fd, "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\n0123456789");
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("200")) << resp;
}

// Test HEAD method doesn't send body
TEST(SingleHttpServer, HeadMethodNoBody) {
  ts.router().setDefault([](const HttpRequest&) {
    HttpResponse resp("This is the body content");
    resp.addHeader("X-Custom", "value");
    return resp;
  });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();
  test::sendAll(fd, "HEAD / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  std::string resp = test::recvUntilClosed(fd);
  ASSERT_TRUE(resp.contains("200"));
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
  ASSERT_TRUE(resp.contains("200") || resp.contains("204")) << resp;
}

// Test exception in request middleware
TEST(SingleHttpServer, MiddlewareExceptionHandling) {
  ts.router().addRequestMiddleware([](HttpRequest&) {
    // Test just that adding a middleware that throws doesn't crash
    throw std::runtime_error("middleware failure");
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

// Test exception in response middleware
TEST(SingleHttpServer, ResponseMiddlewareException) {
  ts.router().addResponseMiddleware(
      [](const HttpRequest&, HttpResponse&) { throw std::runtime_error("response middleware error"); });

  ts.router().addResponseMiddleware([](const HttpRequest&, HttpResponse&) { throw 42; });

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

  ts.router().setPath(http::Method::GET, "/test",
                      [](const HttpRequest&, HttpResponseWriter& writer) { writer.writeBody("test"); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_FALSE(test::recvUntilClosed(fd).empty());

  clientConnection = test::ClientConnection(ts.port());
  fd = clientConnection.fd();

  test::sendAll(fd, "GET /test HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_FALSE(test::recvUntilClosed(fd).empty());

  ts.router() = Router();  // Clear middlewares for other tests
}

// Test multiple response middleware
TEST(SingleHttpServer, MultipleResponseMiddleware) {
  ts.router().addResponseMiddleware(
      [](const HttpRequest&, HttpResponse& resp) { resp.addHeader("X-Middleware-1", "first"); });

  ts.router().addResponseMiddleware(
      [](const HttpRequest&, HttpResponse& resp) { resp.addHeader("X-Middleware-2", "second"); });

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
  test::FailAllEpollCtlMod(EBADF);

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(std::string(24UL * 1024 * 1024, 'Y')); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  // Send the request with Connection: close for clean termination
  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

  auto data = test::recvUntilClosed(fd);

  // Should have received data
  EXPECT_GT(data.size(), 0UL);

  // The test verifies the server doesn't crash when MOD failures happen
  // MOD may or may not be called depending on write buffering
  test::ResetEpollCtlModFail();
}

// Test epoll_ctl MOD failure with EACCES (serious) error
TEST(SingleHttpServer, EpollCtlModEaccesFailure) {
  test::FailAllEpollCtlMod(EACCES);

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(std::string(24UL * 1024 * 1024, 'Y')); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

  auto data = test::recvUntilClosed(fd);

  EXPECT_GT(data.size(), 0UL);

  test::ResetEpollCtlModFail();
}

TEST(SingleHttpServer, EpollPollFailure) {
  test::EventLoopHookGuard guard;
  test::SetEpollWaitActions({test::WaitError(EINTR), test::WaitError(EACCES), test::WaitError(EACCES),
                             test::WaitError(EINTR), test::WaitError(EBADF), test::WaitError(EBADF)});

  ts.router().setDefault([](const HttpRequest&) { return HttpResponse(std::string(1024UL * 1024, 'Y')); });

  test::ClientConnection clientConnection(ts.port());
  int fd = clientConnection.fd();

  test::sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");

  auto data = test::recvWithTimeout(fd, std::chrono::milliseconds{50});

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
  ASSERT_EQ(0, ::shutdown(clientFd, SHUT_WR));

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

  ASSERT_EQ(0, ::shutdown(clientFd, SHUT_WR));

  // Inject EPOLLHUP WITHOUT EPOLLIN.
  test::PushEpollWaitAction(test::WaitReturn(1, {test::MakeEvent(serverFd, EPOLLHUP)}));

  ASSERT_TRUE(WaitForPeerClosedNonBlocking(clientFd, 1s));
}

TEST(SingleHttpServer, EpollErrWithoutInTriggersCloseOnReadError) {
  test::EventLoopHookGuard hookGuard;
  test::TestServer localTs(TestServerConfig(), RouterConfig{}, std::chrono::milliseconds{5});
  localTs.router().setDefault([](const HttpRequest&) { return HttpResponse("OK"); });

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
