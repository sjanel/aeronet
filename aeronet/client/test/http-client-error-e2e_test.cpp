// Error / retry path coverage for HttpClient that a well-behaved aeronet server never produces:
// abrupt close mid-response, a dead pooled keep-alive connection, a silent (timing-out) peer, and a
// hard DNS resolution failure. These drive a tiny hand-rolled raw TCP server so each connection can be
// scripted byte-for-byte (and closed) independently of the HTTP server implementation.
#include <gtest/gtest.h>

#ifdef AERONET_POSIX
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#elifdef AERONET_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/close-native-handle.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/retry-config.hpp"
#include "aeronet/socket-ops.hpp"

namespace aeronet {
namespace {

// Wait until `fd` is readable (or timeoutMs elapses). Uses poll on POSIX and select on Windows, which
// keeps the raw server self-contained without pulling in an event-loop abstraction. Returns true if the
// socket became readable.
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
  pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
  return ::poll(&pfd, 1, timeoutMs) > 0;
#endif
}

// Read until the end of the request header block ("\r\n\r\n"). The tests only issue bodyless GETs, so
// this is enough to know the client finished writing its request before we script the response.
void DrainRequest(NativeHandle fd) {
  std::string buf;
  char tmp[1024];
  for (;;) {
    const auto sz = ::recv(fd, tmp, static_cast<int>(sizeof(tmp)), 0);
    if (sz <= 0) {
      return;
    }
    buf.append(tmp, static_cast<std::size_t>(sz));
    if (buf.contains("\r\n\r\n")) {
      return;
    }
  }
}

// Read and return the request header block (up to and including the terminating "\r\n\r\n").
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
    // SafeSend applies the platform-appropriate no-SIGPIPE flags so writing to a peer that already
    // closed surfaces as an error here instead of killing the test process.
    const int64_t sz = SafeSend(fd, data.data() + off, data.size() - off);
    if (sz <= 0) {
      return;
    }
    off += static_cast<std::size_t>(sz);
  }
}

// Minimal scriptable raw TCP server bound to an ephemeral loopback port. For each accepted connection
// it invokes a user handler with the socket fd and a 0-based connection index, then closes the socket.
class RawServer {
 public:
  using Handler = std::function<void(NativeHandle fd, int connIndex)>;

  // family is AF_INET (default) or AF_INET6. The IPv6 path fails softly (listening() stays false) so a
  // test can GTEST_SKIP() on hosts/CI without IPv6 loopback instead of hard-failing.
  explicit RawServer(Handler handler, int family = AF_INET) : _handler(std::move(handler)) {
#ifdef AERONET_WINDOWS
    EnsureWinsockInitialized();
#endif
    if (family == AF_INET6) {
      setupIpv6();
    } else {
      setupIpv4();
    }
    if (_listenFd != kInvalidHandle) {
      _thread = std::thread([this] { run(); });
    }
  }

  RawServer(const RawServer&) = delete;
  RawServer& operator=(const RawServer&) = delete;
  RawServer(RawServer&&) = delete;
  RawServer& operator=(RawServer&&) = delete;

  ~RawServer() {
    _stop.store(true, std::memory_order_relaxed);
    if (_thread.joinable()) {
      _thread.join();
    }
    if (_listenFd != kInvalidHandle) {
      CloseNativeHandle(_listenFd);
    }
  }

  [[nodiscard]] uint16_t port() const noexcept { return _port; }
  [[nodiscard]] bool listening() const noexcept { return _listenFd != kInvalidHandle; }

 private:
  void setupIpv4() {
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
  }

  // Soft setup on IPv6 loopback: any failure leaves _listenFd == kInvalidHandle so the test skips.
  void setupIpv6() {
    _listenFd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (_listenFd == kInvalidHandle) {
      return;
    }
    int one = 1;
    ::setsockopt(_listenFd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_loopback;
    addr.sin6_port = 0;
    if (::bind(_listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      CloseNativeHandle(_listenFd);
      _listenFd = kInvalidHandle;
      return;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(_listenFd, reinterpret_cast<sockaddr*>(&addr), &len) != 0 || ::listen(_listenFd, 8) != 0) {
      CloseNativeHandle(_listenFd);
      _listenFd = kInvalidHandle;
      return;
    }
    _port = ntohs(addr.sin6_port);
  }

  void run() {
    int idx = 0;
    while (!_stop.load(std::memory_order_relaxed)) {
      if (!WaitReadable(_listenFd, 20)) {
        continue;  // timeout (re-check the stop flag) or interrupted
      }
      const NativeHandle fd = ::accept(_listenFd, nullptr, nullptr);
      if (fd == kInvalidHandle) {
        continue;
      }
      _handler(fd, idx++);
      CloseNativeHandle(fd);
    }
  }

  Handler _handler;
  std::thread _thread;
  std::atomic<bool> _stop{false};
  NativeHandle _listenFd{kInvalidHandle};
  uint16_t _port{0};
};

std::string MakeUrl(uint16_t port, std::string_view path = "/") {
  return "http://127.0.0.1:" + std::to_string(port) + std::string(path);
}

}  // namespace

// A hard DNS resolution failure: .invalid is reserved (RFC 6761) and never resolves, so ConnectTCP
// reports an immediate failure. Exercises connectNew()'s failure branch and the subsequent
// event-loop registration failure on the resulting empty connection.
TEST(HttpClientErrorE2ETest, DnsResolutionFailureReturnsError) {
  HttpClient client;
  auto result = client.get("http://aeronet-nonexistent-host.invalid/");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::connectFailed);
}

// The server promises a 100-byte body but sends 5 bytes then closes: the client hits EOF before the
// body is complete. The parser cannot tell a truncated body from a malformed one (both are an
// unparseable response once no more bytes will arrive), so the truncation surfaces as malformedResponse.
// With retries disabled it is not retried away.
TEST(HttpClientErrorE2ETest, ClosedBeforeCompleteResponseReturnsError) {
  RawServer server([](NativeHandle fd, int) {
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
  });
  HttpClientConfig cfg;  // default retry policy: a fresh-connection post-send failure is never retried
  HttpClient client(cfg);
  auto result = client.get(MakeUrl(server.port()));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::malformedResponse);
}

// The server reads the request but never answers; the client must give up at its request deadline.
TEST(HttpClientErrorE2ETest, ReadTimeoutReturnsError) {
  RawServer server([](NativeHandle fd, int) {
    DrainRequest(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds{600});  // outlive the client's deadline
  });
  HttpClientConfig cfg;
  cfg.requestTimeout = std::chrono::milliseconds{150};
  HttpClient client(cfg);
  auto result = client.get(MakeUrl(server.port()));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::timeout);
}

// First request pools a keep-alive connection that the server then closes; the second request finds the
// pooled connection dead and transparently retries on a fresh one (the stale keep-alive race).
TEST(HttpClientErrorE2ETest, StaleKeepAliveConnectionRetriedOnFreshSucceeds) {
  RawServer server([](NativeHandle fd, int idx) {
    DrainRequest(fd);
    SendAll(fd, idx == 0 ? "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst"
                         : "HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\nsecond");
  });
  HttpClientConfig cfg;
  cfg.requestTimeout = std::chrono::seconds{2};  // safety net so a worst-case stale read cannot hang
  HttpClient client(cfg);

  auto r1 = client.get(MakeUrl(server.port())).value();
  EXPECT_EQ(r1.status(), 200);
  EXPECT_EQ(r1.bodyInMemory(), "first");

  // Give the server's close of connection #0 time to reach the client before we reuse it.
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  auto r2 = client.get(MakeUrl(server.port())).value();
  EXPECT_EQ(r2.status(), 200);
  EXPECT_EQ(r2.bodyInMemory(), "second");
}

// First request pools a keep-alive connection; the server is then torn down entirely. The next request
// finds the pooled connection dead, retries on a fresh connect that is refused, and on the retry path
// drops the now-useless pooled bucket before the final (also failing) attempt.
TEST(HttpClientErrorE2ETest, RetryExhaustionDropsDeadPoolThenReturnsError) {
  auto server = std::make_unique<RawServer>([](NativeHandle fd, int) {
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst");
  });
  const uint16_t port = server->port();

  HttpClientConfig cfg;
  cfg.connectTimeout = std::chrono::milliseconds{300};
  cfg.requestTimeout = std::chrono::seconds{2};
  HttpClient client(cfg);

  auto r1 = client.get(MakeUrl(port)).value();
  EXPECT_EQ(r1.bodyInMemory(), "first");

  server.reset();  // dead pooled connection AND refused fresh connects
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  auto result = client.get(MakeUrl(port));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::connectFailed);
}

// A request whose bytes were fully written must NOT be retried when the response then fails (here: a
// truncated body), even with the default retry budget — otherwise a non-idempotent POST would be silently
// re-submitted. The server must therefore observe exactly one connection.
TEST(HttpClientErrorE2ETest, RequestNotReSentAfterBytesWritten) {
  std::atomic<int> connections{0};
  RawServer server([&](NativeHandle fd, int) {
    connections.fetch_add(1, std::memory_order_relaxed);
    DrainRequest(fd);
    // Promise 100 body bytes but deliver 5 then close: the failure surfaces only after the full request
    // was written.
    SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
  });
  HttpClient client;  // default retry policy: a sent request is never re-submitted
  auto result = client.post(MakeUrl(server.port()), "payload");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::malformedResponse);
  // Leave time for a (wrongful) retry connection to be accepted before asserting.
  std::this_thread::sleep_for(std::chrono::milliseconds{80});
  EXPECT_EQ(connections.load(std::memory_order_relaxed), 1);
}

// The stale keep-alive race must still be handled for non-idempotent methods: a POST that finds its pooled
// connection already closed is transparently sent on a fresh connection (and exactly once).
TEST(HttpClientErrorE2ETest, PostOnStalePooledConnectionUsesFreshConnection) {
  RawServer server([](NativeHandle fd, int idx) {
    DrainRequest(fd);
    SendAll(fd, idx == 0 ? "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
                         : "HTTP/1.1 201 Created\r\nContent-Length: 4\r\n\r\ndone");
  });
  HttpClient client;  // keep-alive on; the free pre-send stale-pool retry applies even to POST
  EXPECT_EQ(client.post(MakeUrl(server.port()), "a").value().status(), 200);  // pools the connection
  std::this_thread::sleep_for(std::chrono::milliseconds{60});                 // let the server close conn #0
  const HttpResponse r2 = client.post(MakeUrl(server.port()), "b").value();   // stale pool -> fresh connection
  EXPECT_EQ(r2.status(), 201);
  EXPECT_EQ(r2.bodyInMemory(), "done");
}

namespace {
// A small backoff config for retry tests: a couple of attempts with a near-zero delay so the tests stay
// fast while still exercising the sleep path (a 1ms sleep is still a real sleep_for call).
RetryConfig FastRetry(uint32_t maxAttempts) {
  RetryConfig retry;
  retry.maxAttempts = maxAttempts;
  retry.baseDelay = std::chrono::milliseconds{1};
  retry.maxDelay = std::chrono::milliseconds{4};
  return retry;
}
}  // namespace

// A retryable status (503) recovered on the *same* kept-alive connection: the server answers 503 then 200
// over one connection, so the client retries by reusing the pooled connection (it is never stale here).
TEST(HttpClientErrorE2ETest, StatusRetryReusesKeepAliveConnection) {
  std::atomic<int> connections{0};
  RawServer server([&](NativeHandle fd, int) {
    connections.fetch_add(1, std::memory_order_relaxed);
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n");
    DrainRequest(fd);  // the retry arrives on the same kept-alive connection
    SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  });
  HttpClientConfig cfg;
  cfg.withRetry(FastRetry(2));  // 429 / 503 are retried by default
  HttpClient client(cfg);

  const auto result = client.get(MakeUrl(server.port()));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->status(), 200);
  EXPECT_EQ(result->bodyInMemory(), "ok");
  EXPECT_EQ(connections.load(std::memory_order_relaxed), 1);  // reused the one connection
}

// A retry budget that runs out hands back the last (retryable) response in the success state -- a 503 is a
// normal HttpResponse, not an HttpClientErrc. `Connection: close` forces each retry onto a fresh connection.
TEST(HttpClientErrorE2ETest, StatusRetryExhaustedReturnsLastResponse) {
  std::atomic<int> connections{0};
  RawServer server([&](NativeHandle fd, int) {
    connections.fetch_add(1, std::memory_order_relaxed);
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  });
  HttpClientConfig cfg;
  cfg.withRetry(FastRetry(2));
  HttpClient client(cfg);

  const auto result = client.get(MakeUrl(server.port()));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->status(), 503);
  EXPECT_EQ(connections.load(std::memory_order_relaxed), 2);  // initial attempt + one retry
}

// A status outside the retry set (500 is not in the default {429, 503}) is returned immediately, untouched.
TEST(HttpClientErrorE2ETest, StatusNotInRetrySetIsNotRetried) {
  std::atomic<int> connections{0};
  RawServer server([&](NativeHandle fd, int) {
    connections.fetch_add(1, std::memory_order_relaxed);
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
  });
  HttpClientConfig cfg;
  cfg.withRetry(FastRetry(3));
  HttpClient client(cfg);

  const auto result = client.get(MakeUrl(server.port()));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->status(), 500);
  EXPECT_EQ(connections.load(std::memory_order_relaxed), 1);
}

// A delta-seconds Retry-After is honored (and capped at maxDelay): with maxDelay = 10ms a "Retry-After: 5"
// is clamped to (effectively) no wait, so the retry still happens promptly and recovers on the same conn.
TEST(HttpClientErrorE2ETest, StatusRetryHonorsRetryAfter) {
  RawServer server([](NativeHandle fd, int) {
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nRetry-After: 5\r\n\r\n");
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\ndone");
  });
  RetryConfig retry = FastRetry(2);
  retry.maxDelay = std::chrono::milliseconds{10};  // Retry-After: 5s is clamped to this
  HttpClientConfig cfg;
  cfg.withRetry(retry);
  HttpClient client(cfg);

  const auto start = std::chrono::steady_clock::now();
  const auto result = client.get(MakeUrl(server.port()));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  ASSERT_TRUE(result);
  EXPECT_EQ(result->status(), 200);
  EXPECT_EQ(result->bodyInMemory(), "done");
  // The 5-second Retry-After was capped to maxDelay (10ms): the whole exchange must finish promptly, not
  // after a literal 5s sleep.
  EXPECT_LT(elapsed, std::chrono::seconds{2});
}

// A post-send failure on an *idempotent* method (GET) is retried only when the caller opts in: the first
// connection truncates its response, the second answers fully.
TEST(HttpClientErrorE2ETest, IdempotentPostSendFailureRetriedWhenEnabled) {
  std::atomic<int> connections{0};
  RawServer server([&](NativeHandle fd, int idx) {
    connections.fetch_add(1, std::memory_order_relaxed);
    DrainRequest(fd);
    SendAll(fd, idx == 0 ? "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort"  // truncated -> malformed
                         : "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
  });
  RetryConfig retry = FastRetry(2);
  retry.retryIdempotentAfterSend = true;
  HttpClientConfig cfg;
  cfg.withRetry(retry);
  HttpClient client(cfg);

  const auto result = client.get(MakeUrl(server.port()));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->status(), 200);
  EXPECT_EQ(result->bodyInMemory(), "hello");
  EXPECT_EQ(connections.load(std::memory_order_relaxed), 2);
}

// A non-idempotent method (POST) is never re-submitted after its bytes were written, even with
// retryIdempotentAfterSend enabled: the truncated response surfaces as an error and the server sees one conn.
TEST(HttpClientErrorE2ETest, NonIdempotentPostSendFailureNotRetried) {
  std::atomic<int> connections{0};
  RawServer server([&](NativeHandle fd, int) {
    connections.fetch_add(1, std::memory_order_relaxed);
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nshort");
  });
  RetryConfig retry = FastRetry(3);
  retry.retryIdempotentAfterSend = true;  // still excludes POST (not idempotent)
  HttpClientConfig cfg;
  cfg.withRetry(retry);
  HttpClient client(cfg);

  const auto result = client.post(MakeUrl(server.port()), "payload");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::malformedResponse);
  std::this_thread::sleep_for(std::chrono::milliseconds{80});
  EXPECT_EQ(connections.load(std::memory_order_relaxed), 1);
}

// A connect failure is a pre-send failure, so it is retried (with backoff) up to the attempt budget before
// surfacing. Jitter is enabled to drive the backoff PRNG. The origin is a torn-down server (refused connects).
TEST(HttpClientErrorE2ETest, ConnectFailureRetriedThenExhausted) {
  auto server = std::make_unique<RawServer>([](NativeHandle fd, int) { DrainRequest(fd); });
  const uint16_t port = server->port();
  server.reset();  // nothing listens on `port` now: every connect is refused
  std::this_thread::sleep_for(std::chrono::milliseconds{50});

  RetryConfig retry = FastRetry(3);
  retry.jitter = 0.5F;  // exercise the jitter PRNG path
  HttpClientConfig cfg;
  cfg.connectTimeout = std::chrono::milliseconds{200};
  cfg.withRetry(retry);
  HttpClient client(cfg);

  const auto result = client.get(MakeUrl(port));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), HttpClientErrc::connectFailed);
}

// A maxAttempts of 0 is treated as 1 (a single attempt): a normal request still succeeds.
TEST(HttpClientErrorE2ETest, ZeroMaxAttemptsTreatedAsSingleAttempt) {
  RawServer server([](NativeHandle fd, int) {
    DrainRequest(fd);
    SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
  });
  RetryConfig retry;
  retry.maxAttempts = 0;
  HttpClientConfig cfg;
  cfg.withRetry(retry);
  HttpClient client(cfg);

  const auto result = client.get(MakeUrl(server.port()));
  ASSERT_TRUE(result);
  EXPECT_EQ(result->status(), 200);
}

// An IPv6 literal authority must be bracketed in the Host header: "Host: [::1]:<port>", never "::1:<port>".
TEST(HttpClientErrorE2ETest, Ipv6LiteralHostHeaderIsBracketed) {
  std::mutex mtx;
  std::string capturedHead;
  RawServer server(
      [&](NativeHandle fd, int) {
        std::string head = ReadRequestHead(fd);
        {
          const std::scoped_lock lock(mtx);
          capturedHead = std::move(head);
        }
        SendAll(fd, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
      },
      AF_INET6);
  if (!server.listening()) {
    GTEST_SKIP() << "no IPv6 loopback available";
  }
  HttpClient client;
  const std::string url = "http://[::1]:" + std::to_string(server.port()) + "/";
  EXPECT_EQ(client.get(url).value().status(), 200);

  const std::scoped_lock lock(mtx);
  const std::string expected = "Host: [::1]:" + std::to_string(server.port()) + "\r\n";
  EXPECT_NE(capturedHead.find(expected), std::string::npos) << "request head was:\n" << capturedHead;
}

}  // namespace aeronet
