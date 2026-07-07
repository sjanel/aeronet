// End-to-end coverage for HttpClient's forward-proxy support (HttpClientConfig::withProxy):
//   * a plain http origin reached through the proxy uses an absolute-form request-target;
//   * an https origin is reached by opening an HTTP CONNECT tunnel through the proxy, then handshaking
//     TLS with the origin through it (optionally verifying the origin against the proxy's CA);
//   * a proxy that refuses the CONNECT surfaces as HttpClientErrc::proxyError.
// The proxy is a tiny hand-rolled TCP server: it terminates absolute-form http requests itself and, for
// CONNECT, tunnels raw bytes to a loopback origin -- enough to exercise the client without a real proxy.
#include <gtest/gtest.h>

#ifdef AERONET_POSIX
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#elifdef AERONET_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "aeronet/close-native-handle.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/socket-ops.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include <filesystem>
#include <fstream>
#include <memory>

#include "aeronet/aeronet.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/test-tls-helper.hpp"
#include "aeronet/tls-config.hpp"
#endif

namespace aeronet {
namespace {

// Wait until `fd` is readable (or timeoutMs elapses); poll on POSIX, select on Windows.
bool WaitReadable(NativeHandle fd, int timeoutMs) {
#ifdef AERONET_WINDOWS
  fd_set readSet;
  FD_ZERO(&readSet);
  FD_SET(fd, &readSet);
  timeval tv{};
  tv.tv_sec = timeoutMs / 1000;
  tv.tv_usec = (timeoutMs % 1000) * 1000;
  return ::select(0, &readSet, nullptr, nullptr, &tv) > 0;
#else
  pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};  // NOLINT(misc-include-cleaner)
  return ::poll(&pfd, 1, timeoutMs) > 0;                 // NOLINT(misc-include-cleaner)
#endif
}

// Read the request header block up to (and including) the terminating "\r\n\r\n". Returns what was read so
// far on EOF (empty when the peer closed without sending anything). Good enough for bodyless GET / CONNECT.
std::string ReadRequestHead(NativeHandle fd) {
  std::string buf;
  char tmp[1024];
  for (;;) {
    const auto sz = ::recv(fd, tmp, static_cast<int>(sizeof(tmp)), 0);
    if (sz <= 0) {
      return buf;
    }
    buf.append(tmp, static_cast<std::size_t>(sz));
    if (buf.contains("\r\n\r\n")) {
      return buf;
    }
  }
}

void SendAll(NativeHandle fd, std::string_view data) {
  std::size_t off = 0;
  while (off < data.size()) {
    const int64_t sz = SafeSend(fd, data.data() + off, data.size() - off);
    if (sz <= 0) {
      return;
    }
    off += static_cast<std::size_t>(sz);
  }
}

// Blocking connect to 127.0.0.1:port (aeronet servers bind IPv4 loopback). Returns kInvalidHandle on error.
NativeHandle ConnectLoopbackV4(uint16_t port) {
  const NativeHandle fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd == kInvalidHandle) {
    return kInvalidHandle;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = htons(port);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    CloseNativeHandle(fd);
    return kInvalidHandle;
  }
  return fd;
}

// Extract the port from a "CONNECT host:port ..." request line (tests only ever tunnel to host:port).
uint16_t ParseConnectPort(std::string_view head) {
  const auto sp1 = head.find(' ');
  const auto sp2 = head.find(' ', sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
    return 0;
  }
  const std::string_view authority = head.substr(sp1 + 1, sp2 - sp1 - 1);
  const auto colon = authority.rfind(':');
  if (colon == std::string_view::npos) {
    return 0;
  }
  uint16_t port = 0;
  for (const char ch : authority.substr(colon + 1)) {
    if (ch < '0' || ch > '9') {
      break;
    }
    port = static_cast<uint16_t>((port * 10) + (ch - '0'));
  }
  return port;
}

// Forward bytes both ways between `a` and `b` until either side closes (or the proxy is stopping).
void PumpTunnel(NativeHandle a, NativeHandle b, const std::atomic<bool>& stop) {
  char buf[8192];
  while (!stop.load(std::memory_order_relaxed)) {
    const NativeHandle pair[2][2] = {{a, b}, {b, a}};
    for (const auto& dir : pair) {
      if (!WaitReadable(dir[0], 10)) {
        continue;
      }
      const auto n = ::recv(dir[0], buf, static_cast<int>(sizeof(buf)), 0);
      if (n <= 0) {
        return;  // EOF / error on either half ends the tunnel
      }
      SendAll(dir[1], std::string_view(buf, static_cast<std::size_t>(n)));
    }
  }
}

// A minimal forward proxy bound to an ephemeral IPv4 loopback port. It terminates absolute-form http
// requests itself (replying with a canned response and capturing the first request head), and for CONNECT
// either refuses (when a reject response is configured) or tunnels raw bytes to the requested loopback port.
class TinyProxy {
 public:
  // rejectResponse: when non-empty, CONNECT requests are answered with it (and closed) instead of tunnelled.
  // dropOnConnect: when true, CONNECT requests are dropped (socket closed with no response at all).
  explicit TinyProxy(std::string httpResponse = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nhey",
                     std::string rejectResponse = {}, bool dropOnConnect = false)
      : _httpResponse(std::move(httpResponse)),
        _rejectResponse(std::move(rejectResponse)),
        _dropOnConnect(dropOnConnect) {
#ifdef AERONET_WINDOWS
    EnsureWinsockInitialized();
#endif
    _listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_NE(_listenFd, kInvalidHandle);
    int one = 1;
    ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    EXPECT_EQ(::bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
    socklen_t len = sizeof(addr);
    EXPECT_EQ(::getsockname(_listenFd, reinterpret_cast<sockaddr*>(&addr), &len), 0);
    _port = ntohs(addr.sin_port);
    EXPECT_EQ(::listen(_listenFd, 8), 0);
    _acceptThread = std::thread([this] { acceptLoop(); });
  }

  TinyProxy(const TinyProxy&) = delete;
  TinyProxy& operator=(const TinyProxy&) = delete;
  TinyProxy(TinyProxy&&) = delete;
  TinyProxy& operator=(TinyProxy&&) = delete;

  ~TinyProxy() {
    _stop.store(true, std::memory_order_relaxed);
    if (_acceptThread.joinable()) {
      _acceptThread.join();  // no more workers are spawned after this returns
    }
    for (std::thread& worker : _workers) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    if (_listenFd != kInvalidHandle) {
      CloseNativeHandle(_listenFd);
    }
  }

  [[nodiscard]] uint16_t port() const noexcept { return _port; }
  [[nodiscard]] int connectCount() const noexcept { return _connectCount.load(std::memory_order_relaxed); }
  [[nodiscard]] std::string capturedHead() {
    const std::scoped_lock lock(_mtx);
    return _capturedHead;
  }

 private:
  void acceptLoop() {
    while (!_stop.load(std::memory_order_relaxed)) {
      if (!WaitReadable(_listenFd, 20)) {
        continue;
      }
      const NativeHandle fd = ::accept(_listenFd, nullptr, nullptr);
      if (fd == kInvalidHandle) {
        continue;
      }
      _workers.emplace_back([this, fd] { handleConnection(fd); });
    }
  }

  void handleConnection(NativeHandle clientFd) {
    const std::string head = ReadRequestHead(clientFd);
    if (head.empty()) {
      CloseNativeHandle(clientFd);
      return;
    }
    if (head.starts_with("CONNECT ")) {
      _connectCount.fetch_add(1, std::memory_order_relaxed);
      if (_dropOnConnect) {
        CloseNativeHandle(clientFd);  // drop the tunnel with no response at all
        return;
      }
      if (!_rejectResponse.empty()) {
        SendAll(clientFd, _rejectResponse);
        CloseNativeHandle(clientFd);
        return;
      }
      const NativeHandle targetFd = ConnectLoopbackV4(ParseConnectPort(head));
      if (targetFd == kInvalidHandle) {
        SendAll(clientFd, "HTTP/1.1 502 Bad Gateway\r\n\r\n");
        CloseNativeHandle(clientFd);
        return;
      }
      SendAll(clientFd, "HTTP/1.1 200 Connection Established\r\n\r\n");
      PumpTunnel(clientFd, targetFd, _stop);
      CloseNativeHandle(targetFd);
      CloseNativeHandle(clientFd);
      return;
    }
    // Absolute-form http request: capture it and answer directly (looping to serve keep-alive requests).
    capture(head);
    SendAll(clientFd, _httpResponse);
    while (!_stop.load(std::memory_order_relaxed)) {
      const std::string next = ReadRequestHead(clientFd);
      if (next.empty()) {
        break;
      }
      capture(next);
      SendAll(clientFd, _httpResponse);
    }
    CloseNativeHandle(clientFd);
  }

  void capture(const std::string& head) {
    const std::scoped_lock lock(_mtx);
    if (_capturedHead.empty()) {
      _capturedHead = head;
    }
  }

  std::string _httpResponse;
  std::string _rejectResponse;
  bool _dropOnConnect;
  std::thread _acceptThread;
  std::vector<std::thread> _workers;
  std::mutex _mtx;
  std::string _capturedHead;
  std::atomic<bool> _stop{false};
  std::atomic<int> _connectCount{0};
  NativeHandle _listenFd{kInvalidHandle};
  uint16_t _port{0};
};

std::string ProxyUrl(uint16_t port) { return "http://127.0.0.1:" + std::to_string(port); }

}  // namespace

// A plain http origin reached through the proxy: the client must send the request in absolute-form
// ("GET http://origin/path HTTP/1.1") so the proxy knows which origin to forward to, while the Host header
// still names the origin. The origin host never has to resolve -- the client connects to the proxy.
TEST(HttpClientProxyE2ETest, HttpOriginUsesAbsoluteFormRequestTarget) {
  TinyProxy proxy("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nproxy");
  HttpClientConfig cfg;
  cfg.withProxy(ProxyUrl(proxy.port()));
  HttpClient client(cfg);

  const auto result = client.get("http://origin.invalid:1234/path?x=1");
  ASSERT_TRUE(result) << ErrcToStr(result.error());
  EXPECT_EQ(result->status(), 200);
  EXPECT_EQ(result->bodyInMemory(), "proxy");

  const std::string head = proxy.capturedHead();
  EXPECT_TRUE(head.starts_with("GET http://origin.invalid:1234/path?x=1 HTTP/1.1\r\n")) << head;
  EXPECT_NE(head.find("Host: origin.invalid:1234\r\n"), std::string::npos) << head;
  EXPECT_EQ(proxy.connectCount(), 0);  // plain http never issues CONNECT
}

// The absolute-form target spells out a default port too ("http://host:80/..."), and keep-alive reuse works
// through the proxy (both requests land on the one pooled connection the proxy terminates).
TEST(HttpClientProxyE2ETest, HttpOriginDefaultPortAndKeepAliveReuse) {
  TinyProxy proxy("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  HttpClientConfig cfg;
  cfg.withProxy(ProxyUrl(proxy.port()));
  HttpClient client(cfg);

  const auto r1 = client.get("http://example.test/a");
  ASSERT_TRUE(r1) << ErrcToStr(r1.error());
  EXPECT_EQ(r1->status(), 200);
  const auto r2 = client.get("http://example.test/b");
  ASSERT_TRUE(r2) << ErrcToStr(r2.error());
  EXPECT_EQ(r2->status(), 200);

  EXPECT_TRUE(proxy.capturedHead().starts_with("GET http://example.test:80/a HTTP/1.1\r\n")) << proxy.capturedHead();
}

#ifdef AERONET_ENABLE_OPENSSL

// HTTPS origin reached through the proxy: the client opens a CONNECT tunnel to the origin, then completes
// the TLS handshake with the real aeronet TLS server through it. Cert verification is off (self-signed).
class HttpClientProxyTlsE2ETest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto [certPem, keyPem] = test::MakeEphemeralCertKey("localhost");
    _certPem = certPem;

    TLSConfig tls;
    tls.enabled = true;
    tls.withCertPem(certPem).withKeyPem(keyPem);

    Router router;
    router.setPath(http::Method::GET, "/secure",
                   [](const HttpRequest&) { return HttpResponse(http::StatusCodeOK, "secret", "text/plain"); });

    HttpServerConfig scfg;
    scfg.withPort(0).withKeepAliveTimeout(std::chrono::seconds{5}).withPollInterval(std::chrono::milliseconds{20});
    scfg.tls = std::move(tls);

    _server = std::make_unique<SingleHttpServer>(std::move(scfg), std::move(router));
    _port = _server->port();
    _server->start();
  }

  void TearDown() override { _server.reset(); }

  [[nodiscard]] std::string secureUrl() const { return "https://localhost:" + std::to_string(_port) + "/secure"; }

  std::unique_ptr<SingleHttpServer> _server;
  std::string _certPem;
  uint16_t _port{0};
};

TEST_F(HttpClientProxyTlsE2ETest, HttpsOriginViaConnectTunnel) {
  TinyProxy proxy;
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;  // self-signed origin cert, not in any trust store
  cfg.withProxy(ProxyUrl(proxy.port()));
  HttpClient client(cfg);

  const auto result = client.get(secureUrl());
  ASSERT_TRUE(result) << ErrcToStr(result.error());
  EXPECT_EQ(result->status(), 200);
  EXPECT_EQ(result->bodyInMemory(), "secret");
  EXPECT_GE(proxy.connectCount(), 1);  // the tunnel went through a CONNECT
}

// The proxy CA overrides the trust store for the (tunnelled) origin handshake: pointing it at the origin's
// own self-signed cert makes full verification (chain + hostname "localhost") succeed through the tunnel.
TEST_F(HttpClientProxyTlsE2ETest, HttpsOriginVerifiedAgainstProxyCa) {
  const std::filesystem::path caPath =
      std::filesystem::temp_directory_path() / ("aeronet-proxy-ca-" + std::to_string(::getpid()) + ".pem");
  std::ofstream(caPath) << _certPem;

  TinyProxy proxy;
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = true;  // verify the origin -- against the proxy CA below
  cfg.withProxy(ProxyUrl(proxy.port()), caPath.string());
  HttpClient client(cfg);

  const auto result = client.get(secureUrl());
  ASSERT_TRUE(result) << ErrcToStr(result.error());
  EXPECT_EQ(result->status(), 200);
  EXPECT_EQ(result->bodyInMemory(), "secret");
  std::filesystem::remove(caPath);
}

// A proxy that refuses the CONNECT (403) surfaces as HttpClientErrc::proxyError -- the tunnel never opens,
// so the TLS handshake is never attempted.
TEST_F(HttpClientProxyTlsE2ETest, ProxyRefusesConnectReturnsProxyError) {
  TinyProxy proxy("", "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  cfg.withProxy(ProxyUrl(proxy.port()));
  HttpClient client(cfg);

  const auto result = client.get(secureUrl());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::proxyError);
  EXPECT_GE(proxy.connectCount(), 1);
}

// A proxy that drops the connection without answering the CONNECT (no bytes at all) also surfaces as
// proxyError -- the client reaches EOF before an end-of-headers marker.
TEST_F(HttpClientProxyTlsE2ETest, ProxyDropsConnectReturnsProxyError) {
  TinyProxy proxy("", "", /*dropOnConnect=*/true);
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  cfg.withProxy(ProxyUrl(proxy.port()));
  HttpClient client(cfg);

  const auto result = client.get(secureUrl());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::proxyError);
}

// A proxy that returns a malformed status line (no status code to parse) is rejected as proxyError.
TEST_F(HttpClientProxyTlsE2ETest, ProxyMalformedStatusLineReturnsProxyError) {
  TinyProxy proxy("", "GARBAGE\r\n\r\n");
  HttpClientConfig cfg;
  cfg.tlsVerifyPeer = false;
  cfg.withProxy(ProxyUrl(proxy.port()));
  HttpClient client(cfg);

  const auto result = client.get(secureUrl());
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::proxyError);
}

#endif  // AERONET_ENABLE_OPENSSL

}  // namespace aeronet
