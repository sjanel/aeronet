#include "../src/upgrade-handshake.hpp"

#include <gtest/gtest.h>

#ifdef AERONET_WINDOWS
#include <ws2tcpip.h>
#else
#include <netdb.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/adaptive-poll-timeout.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/event.hpp"
#include "aeronet/file.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/upgraded-connection.hpp"
#include "raw-test-server.hpp"

#ifdef AERONET_LINUX
// The send() override: blocked and failed writes.
#define AERONET_WANT_SOCKET_OVERRIDES
#include "aeronet/sys-test-support.hpp"
#endif

namespace aeronet::internal {

namespace {

using namespace std::chrono_literals;

constexpr std::string_view kSwitchingProtocols =
    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: tcp\r\nConnection: Upgrade\r\n\r\n";

std::string Url(uint16_t port) { return "ws://127.0.0.1:" + std::to_string(port) + "/up"; }

// A port on which nothing listens.
uint16_t ClosedPort() {
  const test::RawServer server([](NativeHandle /*fd*/, int /*connIndex*/) {});
  return server.port();
}

// The addresses of 127.0.0.1 with each of 'ports', one after the other (freeaddrinfo frees the whole chain).
AddrInfoPtr LoopbackAddresses(std::initializer_list<uint16_t> ports) {
  addrinfo* pHead = nullptr;
  addrinfo** ppNext = &pHead;
  for (const uint16_t port : ports) {
    std::string host = "127.0.0.1";  // followed by its null terminator, writable
    *ppNext = ResolveTCP(std::span<char>(host.data(), host.size()), port).release();
    while (*ppNext != nullptr) {
      ppNext = &(*ppNext)->ai_next;
    }
  }
  return AddrInfoPtr(pHead);
}

// Drive 'handshake' with an event loop of the test, as WebSocketClient does, until it ends.
void Drive(HttpClient& client, UpgradeHandshake& handshake) {
  EventLoop loop(PollTimeoutPolicy{.baseTimeout = 5ms});
  NativeHandle watched = kInvalidHandle;
  EventBmp watchedInterest = 0;
  while (handshake.inProgress()) {
    if (handshake.mayReplaceSocket() && watched != kInvalidHandle) {
      loop.del(watched);
      watched = kInvalidHandle;
    }
    if (watched == kInvalidHandle) {
      ASSERT_TRUE(loop.add(EventLoop::EventFd{handshake.fd(), handshake.interest()}));
      watched = handshake.fd();
      watchedInterest = handshake.interest();
    } else if (watchedInterest != handshake.interest()) {
      ASSERT_TRUE(loop.mod(EventLoop::EventFd{watched, handshake.interest()}));
      watchedInterest = handshake.interest();
    }
    for (const EventLoop::EventFd& event : loop.poll()) {
      if (event.fd == watched && handshake.inProgress()) {
        handshake.advance(client, event.eventBmp);
      }
    }
    if (handshake.inProgress() && SteadyClock::now() >= handshake.deadline()) {
      handshake.expire();
    }
  }
  if (watched != kInvalidHandle) {
    loop.del(watched);
  }
}

// Answers the upgrade request with a 101, then keeps the connection open until the client closes it or the test ends
// (the connections are served one after the other).
class SwitchingServer {
 public:
  SwitchingServer()
      : _server([this](NativeHandle fd, int /*connIndex*/) {
          test::RecvHead(fd);
          test::sendAll(fd, kSwitchingProtocols);
          while (!_done.load()) {
            if (test::WaitReadable(fd, 5)) {
              char byte;
              if (::recv(fd, &byte, 1, 0) <= 0) {
                break;
              }
            }
          }
        }) {}

  SwitchingServer(const SwitchingServer&) = delete;
  SwitchingServer(SwitchingServer&&) = delete;
  SwitchingServer& operator=(const SwitchingServer&) = delete;
  SwitchingServer& operator=(SwitchingServer&&) = delete;

  ~SwitchingServer() { _done.store(true); }

  [[nodiscard]] uint16_t port() const noexcept { return _server.port(); }

 private:
  std::atomic<bool> _done{false};
  test::RawServer _server;
};

// Keeps the connections open without answering, until the test ends.
class SilentServer {
 public:
  explicit SilentServer(bool readRequest = true)
      : _server([this, readRequest](NativeHandle fd, int /*connIndex*/) {
          if (readRequest) {
            test::RecvHead(fd);
          }
          while (!_done.load()) {
            std::this_thread::sleep_for(5ms);
          }
        }) {}

  SilentServer(const SilentServer&) = delete;
  SilentServer(SilentServer&&) = delete;
  SilentServer& operator=(const SilentServer&) = delete;
  SilentServer& operator=(SilentServer&&) = delete;

  ~SilentServer() { _done.store(true); }

  [[nodiscard]] uint16_t port() const noexcept { return _server.port(); }

 private:
  std::atomic<bool> _done{false};
  test::RawServer _server;
};

HttpClientConfig ShortTimeouts() {
  HttpClientConfig config;
  config.connectTimeout = 200ms;
  config.requestTimeout = 200ms;
  return config;
}

}  // namespace

TEST(UpgradeHandshake, StepsThroughThePhasesInAnEventLoop) {
  const SwitchingServer server;
  HttpClient client;
  UpgradeHandshake handshake(client.makeUpgradeRequest(Url(server.port()), "tcp"));
  EXPECT_EQ(handshake.phase(), UpgradeHandshake::Phase::Idle);
  handshake.start(client, "\r\nX-Attempt: 1");
  EXPECT_EQ(handshake.phase(), UpgradeHandshake::Phase::Resolve);
  EXPECT_EQ(handshake.fd(), kInvalidHandle);
  EXPECT_EQ(std::string_view(handshake.hostToResolve(client).data(), handshake.hostToResolve(client).size()),
            "127.0.0.1");
  EXPECT_EQ(handshake.portToResolve(client), server.port());
  handshake.onResolved(client, LoopbackAddresses({server.port()}));
  Drive(client, handshake);
  ASSERT_EQ(handshake.phase(), UpgradeHandshake::Phase::Done);
  EXPECT_EQ(handshake.response().status(), http::StatusCodeSwitchingProtocols);
  // The extra lines are only added on the wire.
  EXPECT_FALSE(handshake.request().hasHeader("x-attempt"));
  {
    const UpgradedConnection connection = handshake.takeConnection();
    EXPECT_TRUE(connection.valid());
    EXPECT_TRUE(connection.received.empty());
  }

  // A new attempt with the same request.
  handshake.reset();
  EXPECT_EQ(handshake.phase(), UpgradeHandshake::Phase::Idle);
  handshake.reset();  // nothing to release
  handshake.start(client, {});
  handshake.onResolved(client, LoopbackAddresses({server.port()}));
  Drive(client, handshake);
  EXPECT_EQ(handshake.phase(), UpgradeHandshake::Phase::Done);
}

TEST(UpgradeHandshake, ConnectFallsBackToTheNextAddress) {
  const SwitchingServer server;
  HttpClient client;
  UpgradeHandshake handshake(client.makeUpgradeRequest(Url(server.port()), "tcp"));
  handshake.start(client, {});
  // A refused connect (pending, or failing at once), then the server.
  handshake.onResolved(client, LoopbackAddresses({ClosedPort(), ClosedPort(), server.port()}));
  Drive(client, handshake);
  ASSERT_EQ(handshake.phase(), UpgradeHandshake::Phase::Done);
  EXPECT_EQ(handshake.response().status(), http::StatusCodeSwitchingProtocols);
}

TEST(UpgradeHandshake, ConnectFailsWhenEveryAddressFails) {
  HttpClient client;
  UpgradeHandshake handshake(client.makeUpgradeRequest(Url(ClosedPort()), "tcp"));
  handshake.start(client, {});
  handshake.onResolved(client, LoopbackAddresses({ClosedPort(), ClosedPort()}));
  Drive(client, handshake);
  ASSERT_EQ(handshake.phase(), UpgradeHandshake::Phase::Failed);
  EXPECT_EQ(handshake.error(), HttpClientErrc::connectFailed);
}

TEST(UpgradeHandshake, FailedLastConnectLeavesTheEventLoopClean) {
  // Listening before the upgrade: the socket of the request below reuses the number of the closed socket of the
  // upgrade.
  test::RawServer server([](NativeHandle fd, int /*connIndex*/) {
    test::RecvHead(fd);
    test::sendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  });
  HttpClientConfig config = ShortTimeouts();
  config.connectTimeout = 10s;  // Windows retries a refused connect for about 2 s
  HttpClient client(config);
  // The connect of the only address fails once the event loop watched it.
  HttpUpgradeResult failed = client.upgrade(client.makeUpgradeRequest(Url(ClosedPort()), "tcp"));
  ASSERT_FALSE(failed);
  EXPECT_EQ(failed.error(), HttpClientErrc::connectFailed);
  // The event loop watches the socket of the next exchange afresh.
  const HttpClientResult result = client.get("http://127.0.0.1:" + std::to_string(server.port()) + "/");
  ASSERT_TRUE(result) << ErrcToStr(result.error());
  EXPECT_EQ(result->status(), http::StatusCodeOK);
  EXPECT_EQ(result->bodyInMemory(), "ok");
}

TEST(UpgradeHandshake, ResolutionFailure) {
  HttpClient client;
  UpgradeHandshake handshake(client.makeUpgradeRequest("ws://no-such-host.invalid/up", "tcp"));
  handshake.start(client, {});
  handshake.onResolved(client, nullptr);
  ASSERT_EQ(handshake.phase(), UpgradeHandshake::Phase::Failed);
  EXPECT_EQ(handshake.error(), HttpClientErrc::connectFailed);
  EXPECT_FALSE(handshake.inProgress());
}

TEST(UpgradeHandshake, CapturedRequestBodyIsSent) {
  static constexpr std::size_t kBodySize = 4000;
  std::atomic<std::size_t> nbBodyBytes{0};
  std::atomic<bool> done{false};
  test::RawServer server([&nbBodyBytes, &done](NativeHandle fd, int /*connIndex*/) {
    const std::string head = test::RecvHead(fd);
    std::size_t received = head.size() - (head.find("\r\n\r\n") + 4U);
    char buf[4096];
    while (received < kBodySize) {
      const auto nb = ::recv(fd, buf, sizeof(buf), 0);
      if (nb <= 0) {
        break;
      }
      received += static_cast<std::size_t>(nb);
    }
    nbBodyBytes.store(received);
    test::sendAll(fd, kSwitchingProtocols);
    while (!done.load()) {
      std::this_thread::sleep_for(5ms);
    }
  });
  HttpClient client;
  HttpRequest req = client.makeUpgradeRequest(Url(server.port()), "tcp");
  req.body(std::string(kBodySize, 'b'));
  HttpUpgradeResult result = client.upgrade(std::move(req));
  done.store(true);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->response.status(), http::StatusCodeSwitchingProtocols);
  EXPECT_EQ(nbBodyBytes.load(), kBodySize);
}

TEST(UpgradeHandshake, RequestWithAFileBodyOrTrailersThrows) {
  HttpClient client;
  {
    const test::ScopedTempDir tmpDir;
    const test::ScopedTempFile tmp(tmpDir, "file body");
    HttpRequest req = client.makeUpgradeRequest(Url(ClosedPort()), "tcp");
    req.file(File(tmp.filePath().string()));
    EXPECT_THROW(static_cast<void>(client.upgrade(std::move(req))), std::invalid_argument);
  }
  HttpRequest req = client.makeUpgradeRequest(Url(ClosedPort()), "tcp");
  req.body("body");
  req.trailerAddLine("x-checksum", "1");
  EXPECT_THROW(static_cast<void>(client.upgrade(std::move(req))), std::invalid_argument);
}

TEST(UpgradeHandshake, ProxyRefusingTheTunnel) {
  for (const std::string_view answer : {std::string_view{"HTTP/1.1 403 Forbidden\r\n\r\n"}, std::string_view{}}) {
    test::RawServer proxy([answer](NativeHandle fd, int /*connIndex*/) {
      test::RecvHead(fd);
      test::sendAll(fd, answer);  // nothing: the proxy closes the connection
    });
    HttpClientConfig config;
    config.withProxy("http://127.0.0.1:" + std::to_string(proxy.port()));
    HttpClient client(config);
    HttpUpgradeResult result = client.upgrade(client.makeUpgradeRequest(Url(ClosedPort()), "tcp"));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), HttpClientErrc::proxyError);
  }
}

TEST(UpgradeHandshake, ProxyNotAnsweringTimesOut) {
  const SilentServer proxy;
  HttpClientConfig config = ShortTimeouts();
  config.withProxy("http://127.0.0.1:" + std::to_string(proxy.port()));
  HttpClient client(config);
  HttpUpgradeResult result = client.upgrade(client.makeUpgradeRequest(Url(ClosedPort()), "tcp"));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::timeout);
}

TEST(UpgradeHandshake, ServerNotAnsweringTimesOut) {
  const SilentServer server;
  HttpClient client(ShortTimeouts());
  HttpUpgradeResult result = client.upgrade(client.makeUpgradeRequest(Url(server.port()), "tcp"));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::timeout);
}

TEST(UpgradeHandshake, MalformedAndTooLargeResponses) {
  struct Case {
    std::string_view response;
    std::size_t maxResponseBytes;
    HttpClientErrc error;
  };
  static constexpr Case kCases[]{
      {"garbage\r\n\r\n", 1024U, HttpClientErrc::malformedResponse},
      {"HTTP/1.1 403 Forbidden\r\nContent-Length: 500\r\n\r\n", 100U, HttpClientErrc::responseTooLarge},
  };
  for (const Case& testCase : kCases) {
    test::RawServer server([&testCase](NativeHandle fd, int /*connIndex*/) {
      test::RecvHead(fd);
      test::sendAll(fd, std::string(testCase.response) + std::string(500, 'x'));
    });
    HttpClientConfig config;
    config.maxResponseBytes = testCase.maxResponseBytes;
    HttpClient client(config);
    HttpUpgradeResult result = client.upgrade(client.makeUpgradeRequest(Url(server.port()), "tcp"));
    ASSERT_FALSE(result) << testCase.response;
    EXPECT_EQ(result.error(), testCase.error) << testCase.response;
  }
}

TEST(UpgradeHandshake, ResponseInSeveralReads) {
  std::atomic<bool> done{false};
  test::RawServer server([&done](NativeHandle fd, int /*connIndex*/) {
    test::RecvHead(fd);
    const std::size_t half = kSwitchingProtocols.size() / 2U;
    test::sendAll(fd, kSwitchingProtocols.substr(0, half));
    std::this_thread::sleep_for(20ms);  // the client parses the first half alone, then waits for the rest
    test::sendAll(fd, kSwitchingProtocols.substr(half));
    while (!done.load()) {
      std::this_thread::sleep_for(5ms);
    }
  });
  HttpClient client;
  HttpUpgradeResult result = client.upgrade(client.makeUpgradeRequest(Url(server.port()), "tcp"));
  done.store(true);
  ASSERT_TRUE(result);
  EXPECT_EQ(result->response.status(), http::StatusCodeSwitchingProtocols);
}

#ifdef AERONET_ENABLE_OPENSSL
TEST(UpgradeHandshake, TlsFailures) {
  {
    // A server closing the connection instead of answering the ClientHello.
    test::RawServer server([](NativeHandle fd, int /*connIndex*/) {
      char buf[512];
      static_cast<void>(::recv(fd, buf, sizeof(buf), 0));
    });
    HttpClient client;
    HttpUpgradeResult result =
        client.upgrade(client.makeUpgradeRequest("wss://127.0.0.1:" + std::to_string(server.port()) + "/up", "tcp"));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), HttpClientErrc::tlsError);
  }
  {
    // A server never answering the ClientHello.
    const SilentServer server(false);
    HttpClient client(ShortTimeouts());
    HttpUpgradeResult result =
        client.upgrade(client.makeUpgradeRequest("wss://127.0.0.1:" + std::to_string(server.port()) + "/up", "tcp"));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), HttpClientErrc::timeout);
  }
}
#endif

#ifdef AERONET_LINUX
TEST(UpgradeHandshake, ConnectNotCompletingFailsAtTheConnectTimeout) {
  // A listening socket whose accept queue is full: the SYN of the next connections is dropped.
  const NativeHandle listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_NE(listenFd, kInvalidHandle);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::bind(listenFd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);
  socklen_t len = sizeof(addr);
  ASSERT_EQ(::getsockname(listenFd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
  ASSERT_EQ(::listen(listenFd, 0), 0);
  const NativeHandle filler = ::socket(AF_INET, SOCK_STREAM, 0);
  ASSERT_EQ(::connect(filler, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);

  HttpClient client(ShortTimeouts());
  const auto start = std::chrono::steady_clock::now();
  HttpUpgradeResult result = client.upgrade(client.makeUpgradeRequest(Url(ntohs(addr.sin_port)), "tcp"));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::connectFailed);  // as a regular request
  EXPECT_LT(std::chrono::steady_clock::now() - start, 5s);
  ::close(filler);
  ::close(listenFd);
}

TEST(UpgradeHandshake, BlockedAndFailedRequestWrites) {
  {
    const SwitchingServer server;
    HttpClient client;
    const test::QueueResetGuard resetGuard(test::g_send_actions);
    test::PushSendAction({-1, EAGAIN});  // the request waits for the writability of the socket
    HttpUpgradeResult result = client.upgrade(client.makeUpgradeRequest(Url(server.port()), "tcp"));
    ASSERT_TRUE(result);
    EXPECT_EQ(result->response.status(), http::StatusCodeSwitchingProtocols);
  }
  {
    const SilentServer server;
    HttpClient client;
    const test::QueueResetGuard resetGuard(test::g_send_actions);
    test::PushSendAction({-1, ECONNRESET});
    HttpUpgradeResult result = client.upgrade(client.makeUpgradeRequest(Url(server.port()), "tcp"));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), HttpClientErrc::writeError);
  }
}

TEST(UpgradeHandshake, AnswerBeforeTheEndOfTheRequest) {
  // A server answering once it read the head, then neither reading the body (far larger than the socket buffers) nor
  // closing the connection. A refusal ends the attempt with its response, a switch fails it: the rest of the request
  // would be taken for bytes of the new protocol.
  static constexpr std::string_view kRefusal = "HTTP/1.1 413 Content Too Large\r\nContent-Length: 4\r\n\r\nbig!";
  for (const std::string_view answer : {kRefusal, kSwitchingProtocols}) {
    std::atomic<bool> done{false};
    test::RawServer server([answer, &done](NativeHandle fd, int /*connIndex*/) {
      test::RecvHead(fd);
      test::sendAll(fd, answer);
      while (!done.load()) {
        std::this_thread::sleep_for(5ms);
      }
    });
    HttpClientConfig config;
    config.requestTimeout = 5s;
    HttpClient client(config);
    HttpRequest req = client.makeUpgradeRequest(Url(server.port()), "tcp");
    req.body(std::string(32UL << 20U, 'b'));
    const auto start = std::chrono::steady_clock::now();
    HttpUpgradeResult result = client.upgrade(std::move(req));
    done.store(true);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 4s) << answer;  // not a timeout
    if (answer == kRefusal) {
      ASSERT_TRUE(result);
      EXPECT_EQ(result->response.status(), http::StatusCodePayloadTooLarge);
      EXPECT_EQ(result->response.bodyInMemory(), "big!");
      EXPECT_FALSE(result->connection.valid());
    } else {
      ASSERT_FALSE(result);
      EXPECT_EQ(result.error(), HttpClientErrc::malformedResponse);
    }
  }
}

TEST(UpgradeHandshake, BlockedAndFailedProxyWrites) {
  {
    // A proxy opening the tunnel, then answering the upgrade request itself.
    std::atomic<bool> done{false};
    test::RawServer proxy([&done](NativeHandle fd, int /*connIndex*/) {
      test::RecvHead(fd);
      test::sendAll(fd, "HTTP/1.1 200 Connection Established\r\n\r\n");
      test::RecvHead(fd);
      test::sendAll(fd, kSwitchingProtocols);
      while (!done.load()) {
        std::this_thread::sleep_for(5ms);
      }
    });
    HttpClientConfig config;
    config.withProxy("http://127.0.0.1:" + std::to_string(proxy.port()));
    HttpClient client(config);
    const test::QueueResetGuard resetGuard(test::g_send_actions);
    test::PushSendAction({-1, EAGAIN});  // the CONNECT request waits for the writability of the socket
    HttpUpgradeResult result = client.upgrade(client.makeUpgradeRequest(Url(ClosedPort()), "tcp"));
    done.store(true);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->response.status(), http::StatusCodeSwitchingProtocols);
  }
  {
    const SilentServer proxy(false);
    HttpClientConfig config;
    config.withProxy("http://127.0.0.1:" + std::to_string(proxy.port()));
    HttpClient client(config);
    const test::QueueResetGuard resetGuard(test::g_send_actions);
    test::PushSendAction({-1, ECONNRESET});
    HttpUpgradeResult result = client.upgrade(client.makeUpgradeRequest(Url(ClosedPort()), "tcp"));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), HttpClientErrc::proxyError);
  }
}

TEST(UpgradeHandshake, ConnectCompletingAtOnce) {
  // The connect override reports the connection as established at once, without connecting the socket: the request is
  // written right away, without waiting for the writability of the socket, and fails.
  HttpClient client;
  UpgradeHandshake handshake(client.makeUpgradeRequest(Url(ClosedPort()), "tcp"));
  handshake.start(client, {});
  AddrInfoPtr addresses = LoopbackAddresses({ClosedPort()});
  const test::QueueResetGuard resetGuard(test::g_connect_actions);
  test::PushConnectAction({0, 0});
  handshake.onResolved(client, std::move(addresses));
  ASSERT_EQ(handshake.phase(), UpgradeHandshake::Phase::Failed);
  EXPECT_EQ(handshake.error(), HttpClientErrc::writeError);
}
#endif

}  // namespace aeronet::internal
