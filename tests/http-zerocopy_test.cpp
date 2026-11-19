#include <gtest/gtest.h>

#ifdef AERONET_POSIX
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>  // NOLINT(misc-include-cleaner) IFF_LOOPBACK, IFF_UP
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>  // NOLINT(misc-include-cleaner) socklen_t, SOL_SOCKET, SO_SNDBUF
#include <sys/types.h>
#elifdef AERONET_WINDOWS
#include <winsock2.h>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/base-fd.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/zerocopy-mode.hpp"

using namespace std::chrono_literals;

namespace aeronet {

namespace {
test::TestServer ts;

// Receive buffer sizes for slow reader tests. The minimal one (raised to the kernel minimum), set before connecting,
// keeps the advertised window small: a few KiB of response then stay in the server send queue, still referencing the
// server buffers. Switching to the large one afterwards quickly delivers the rest.
// Some kernels (Linux 7.0) then deliver MSG_ZEROCOPY sends by a few hundred bytes per delayed ACK or retransmission
// timeout, even once the buffer is enlarged. The tests queueing large zerocopy responses rather fix the server send
// buffer (kSmallSendBufferBytes), the client receiving with the large buffer from the start: the server socket fills
// all the same, the kernel automatic tuning no longer growing its send buffer to the size of the response.
constexpr int kMinimalReceiveBufferSize = 1;
constexpr int kLargeReceiveBufferSize = 65536;
constexpr std::uint32_t kSmallSendBufferBytes = 64U << 10U;

}  // namespace

// ============================
// Zerocopy mode integration tests
// ============================

TEST(ZerocopyMode, LargeResponseWithZerocopyOpportunistic) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Opportunistic);
    cfg.withZerocopyMinBytes(16U * 1024U);
  });

  // Create a payload larger than the zerocopy threshold
  constexpr std::size_t kLargePayloadSize = 32UL * 1024;  // 32 KB
  const std::string largePayload(kLargePayloadSize, 'Z');

  ts.resetRouterAndGet().setPath(http::Method::GET, "/large", [&largePayload](const HttpRequestView& req) {
    return req.makeResponse(largePayload);
  });

  const auto before = ts.server.stats();
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/large";
  auto resp = test::requestOrThrow(ts.port(), opt);

  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(resp.ends_with(largePayload));
  // The kernel copies the data sent to a loopback peer anyway: no MSG_ZEROCOPY for it in Opportunistic mode.
  EXPECT_EQ(ts.server.stats().zerocopySends, before.zerocopySends);
}

TEST(ZerocopyMode, LargeResponseWithZerocopyDisabled) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Disabled);
    cfg.withZerocopyMinBytes(16U * 1024U);
  });

  constexpr std::size_t kLargePayloadSize = 32UL * 1024;  // 32 KB
  const std::string largePayload(kLargePayloadSize, 'D');

  ts.resetRouterAndGet().setPath(http::Method::GET, "/large-disabled", [&largePayload](const HttpRequestView& req) {
    return req.makeResponse(largePayload);
  });

  const auto before = ts.server.stats();
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/large-disabled";
  auto resp = test::requestOrThrow(ts.port(), opt);

  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(resp.ends_with(largePayload));
  EXPECT_EQ(ts.server.stats().zerocopySends, before.zerocopySends);
}

TEST(ZerocopyMode, LargeResponseWithZerocopyEnabled) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(16U * 1024U);
  });

  constexpr std::size_t kLargePayloadSize = 32UL * 1024;  // 32 KB
  const std::string largePayload(kLargePayloadSize, 'E');

  ts.resetRouterAndGet().setPath(http::Method::GET, "/large-enabled", [&largePayload](const HttpRequestView& req) {
    return req.makeResponse(largePayload);
  });

  const auto before = ts.server.stats();
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/large-enabled";
  auto resp = test::requestOrThrow(ts.port(), opt);

  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(resp.ends_with(largePayload));
#ifdef AERONET_LINUX
  const auto after = ts.server.stats();
  if (after.zerocopySends == before.zerocopySends &&
      after.zerocopyNoBufferSpaceFallbacks != before.zerocopyNoBufferSpaceFallbacks) {
    GTEST_SKIP() << "MSG_ZEROCOPY sends fail with ENOBUFS here (RLIMIT_MEMLOCK)";
  }
  EXPECT_GT(after.zerocopySends, before.zerocopySends);
  EXPECT_GE(after.zerocopyBytes - before.zerocopyBytes, kLargePayloadSize);
#endif
}

TEST(ZerocopyMode, SmallResponseDoesNotUseZerocopy) {
  // Responses below zerocopyMinBytes (128 KiB by default) bypass zerocopy even when enabled
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withZerocopyMode(ZerocopyMode::Enabled); });

  const std::string smallPayload(64UL * 1024, 's');

  ts.resetRouterAndGet().setPath(http::Method::GET, "/small", [&smallPayload](const HttpRequestView& req) {
    return req.makeResponse(smallPayload);
  });

  const auto before = ts.server.stats();
  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/small";
  auto resp = test::requestOrThrow(ts.port(), opt);

  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(resp.ends_with(smallPayload));
  EXPECT_EQ(ts.server.stats().zerocopySends, before.zerocopySends);
}

// ============================
// Zerocopy stress tests — data integrity under backpressure
// ============================
#ifdef AERONET_LINUX
TEST(ZerocopyMode, ForcedModeSmallPayload) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(1U);
  });

  const std::string payload = "ForcedZerocopySmall";

  ts.resetRouterAndGet().setPath(http::Method::GET, "/forced-small",
                                 [&payload](const HttpRequestView& req) { return req.makeResponse(payload); });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/forced-small";
  auto resp = test::requestOrThrow(ts.port(), opt);

  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200"));
  EXPECT_TRUE(resp.ends_with(payload));
}

TEST(ZerocopyMode, StressLargePayloadDataIntegrity) {
  // Stress test: repeated requests with large payloads to exercise zerocopy + backpressure.
  // Reproduces data corruption seen under sustained zerocopy with virtual network devices (K8s).
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(1U);
  });

  // 1 MB payload with deterministic pattern to verify data integrity
  constexpr std::size_t kPayloadSize = 1UL << 20U;
  std::string largePayload;
  largePayload.reserve(kPayloadSize);
  for (std::size_t idx = 0; idx < kPayloadSize; ++idx) {
    largePayload.push_back(static_cast<char>('A' + (idx % 26)));
  }

  ts.resetRouterAndGet().setPath(http::Method::GET, "/stress", [&largePayload](const HttpRequestView& req) {
    return req.makeResponse(largePayload);
  });

  test::RequestOptions opt;
  opt.method = "GET";
  opt.target = "/stress";
  opt.recvTimeout = std::chrono::milliseconds{5000};

  constexpr int kIterations = 50;
  for (int iter = 0; iter < kIterations; ++iter) {
    auto resp = test::requestOrThrow(ts.port(), opt);
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << "iteration " << iter;
    ASSERT_TRUE(resp.ends_with(largePayload)) << "data corruption at iteration " << iter;
  }
}

TEST(ZerocopyMode, StressConcurrentLargePayloads) {
  // Concurrent stress test: multiple threads making large payload requests simultaneously.
  // Uses Opportunistic mode which auto-disables MSG_ZEROCOPY on loopback connections.
  // This test verifies large-payload concurrency under the zerocopy-enabled configuration
  // (even though loopback falls back to regular write). Single-threaded zerocopy data integrity
  // is covered by StressLargePayloadDataIntegrity and StressVaryingPayloadSizes, and forced zerocopy
  // under concurrent connections by ConcurrentClosingConnectionsReceiveTheirOwnPayload.
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) { cfg.withZerocopyMode(ZerocopyMode::Opportunistic); });

  constexpr std::size_t kPayloadSize = 512UL * 1024;  // 512 KB
  std::string largePayload;
  largePayload.reserve(kPayloadSize);
  for (std::size_t idx = 0; idx < kPayloadSize; ++idx) {
    largePayload.push_back(static_cast<char>('0' + (idx % 10)));
  }

  ts.resetRouterAndGet().setPath(http::Method::GET, "/concurrent-stress", [&largePayload](const HttpRequestView& req) {
    return req.makeResponse(largePayload);
  });

  constexpr int kThreads = 8;
  constexpr int kRequestsPerThread = 20;

  vector<std::thread> threads;
  std::atomic<int> failures{0};

  threads.reserve(kThreads);
  for (int th = 0; th < kThreads; ++th) {
    threads.emplace_back([&] {
      test::RequestOptions opt;
      opt.method = "GET";
      opt.target = "/concurrent-stress";
      opt.recvTimeout = std::chrono::milliseconds{10000};

      for (int req = 0; req < kRequestsPerThread; ++req) {
        try {
          auto resp = test::requestOrThrow(ts.port(), opt);
          if (!resp.starts_with("HTTP/1.1 200") || !resp.ends_with(largePayload)) {
            ++failures;
          }
        } catch (...) {
          ++failures;
        }
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  EXPECT_EQ(failures.load(), 0) << "Some requests had data corruption or failures under concurrency";
}

namespace {
// Expects the server to reset the connection of a client reading nothing from it (reading would make its zerocopy sends
// progress): waits for the reset without reading, then checks that only a part of the payload was received before it.
// The client only notices the reset when it sends something (the reset of its zero window is not acceptable): here the
// window update of its first read, which the server answers with another reset.
void ExpectResetBeforeReading(const SingleHttpServer& server, uint64_t resetsBefore, NativeHandle fd,
                              std::size_t payloadSize) {
  for (int attempt = 0; attempt < 500 && server.stats().zerocopyResetConnections == resetsBefore; ++attempt) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_GT(server.stats().zerocopyResetConnections, resetsBefore) << "no reset within 5 s";

  test::setRecvTimeout(fd, std::chrono::seconds{5});
  std::size_t received = 0;
  char buf[1U << 16U];
  ssize_t ret;
  while ((ret = ::recv(fd, buf, sizeof(buf), 0)) > 0) {
    received += static_cast<std::size_t>(ret);
  }
  EXPECT_EQ(ret, -1) << "connection closed gracefully after " << received << " bytes";
  EXPECT_EQ(errno, ECONNRESET);
  EXPECT_LT(received, payloadSize);
}
}  // namespace

// Closing a connection (here Connection: close) while the kernel still sends its response with MSG_ZEROCOPY must not
// release the response buffers: they would be reused for other responses while the kernel still reads them, sending
// bytes of another response to this client. Each thread has its own payload, so such bytes cannot go unnoticed.
TEST(ZerocopyMode, ConcurrentClosingConnectionsReceiveTheirOwnPayload) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(1U);
  });

  constexpr uint32_t kThreads = 8;
  constexpr int kRequestsPerThread = 20;
  constexpr std::size_t kPayloadSize = 512UL * 1024;
  vector<std::string> payloads;
  payloads.reserve(kThreads);
  for (uint32_t th = 0; th < kThreads; ++th) {
    payloads.emplace_back(kPayloadSize, static_cast<char>('a' + th));
  }

  auto router = ts.resetRouterAndGet();
  for (uint32_t th = 0; th < kThreads; ++th) {
    router.setPath(http::Method::GET, "/own-payload-" + std::to_string(th),
                   [&payloads, th](const HttpRequestView& req) { return req.makeResponse(payloads[th]); });
  }

  std::atomic<int> failures{0};
  vector<std::thread> threads;
  threads.reserve(kThreads);
  for (uint32_t th = 0; th < kThreads; ++th) {
    threads.emplace_back([&payloads, &failures, th] {
      test::RequestOptions opt;  // Connection: close
      opt.method = "GET";
      opt.target = "/own-payload-" + std::to_string(th);
      opt.recvTimeout = std::chrono::milliseconds{10000};
      for (int req = 0; req < kRequestsPerThread; ++req) {
        try {
          const auto resp = test::requestOrThrow(ts.port(), opt);
          if (!resp.starts_with("HTTP/1.1 200") || !resp.ends_with(payloads[th])) {
            ++failures;
          }
        } catch (...) {
          ++failures;
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(failures.load(), 0) << "Some responses were corrupted or truncated";
}

// A client half-closing its connection (EOF for the server) while a zerocopy response is still being sent receives
// the whole response: the server closes once the zerocopy sends completed instead of aborting them.
TEST(ZerocopyMode, HalfClosingClientReceivesWholeResponse) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(1U);
    cfg.withSocketSendBufferBytes(kSmallSendBufferBytes);
  });
  // Several times the server send buffer and the client receive buffer together.
  const std::string payload(1UL << 20U, 'h');
  ts.resetRouterAndGet().setPath(http::Method::GET, "/half-close",
                                 [&payload](const HttpRequestView& req) { return req.makeResponse(payload); });

  test::ClientConnection sock(ts.port(), 500ms, kLargeReceiveBufferSize);
  const NativeHandle fd = sock.fd();
  test::sendAll(fd, "GET /half-close HTTP/1.1\r\nhost: h\r\n\r\n");
  // Half-close once the response started, while most of it is still in the server send queue.
  test::setRecvTimeout(fd, std::chrono::seconds{5});
  std::string resp;
  ASSERT_GT(test::RecvAppend(fd, resp, 512), 0);
  ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  ASSERT_TRUE(ShutdownWrite(fd));
  std::this_thread::sleep_for(std::chrono::milliseconds{50});  // let the server observe the EOF first
  resp += test::recvUntilClosed(fd);
  EXPECT_TRUE(resp.ends_with(payload)) << "received " << resp.size() << " bytes";
}

// A connection that the server has to close while some of its zerocopy sends are still in flight (here at keep-alive
// timeout, the client reading nothing) is reset: the kernel must not keep sending from buffers released at close.
TEST(ZerocopyMode, ForcedCloseWithZerocopySendsInFlightResetsConnection) {
  const auto before = ts.server.stats();
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(1U);
    cfg.withKeepAliveTimeout(std::chrono::milliseconds{100});
  });
  // Small enough for the server send buffer to take it all (a single send, its buffer then retained), but far too large
  // for the client receive buffer.
  const std::string payload(256UL << 10U, 'r');
  ts.resetRouterAndGet().setPath(http::Method::GET, "/stalled-reader",
                                 [&payload](const HttpRequestView& req) { return req.makeResponse(payload); });

  test::ClientConnection sock(ts.port(), 500ms, kMinimalReceiveBufferSize);
  test::sendAll(sock.fd(), "GET /stalled-reader HTTP/1.1\r\nhost: h\r\n\r\n");
  ExpectResetBeforeReading(ts.server, before.zerocopyResetConnections, sock.fd(), payload.size());
}

namespace {

// Receives on fd, reading at most 64 KiB every 'pause'. Returns what was received until it ends with
// 'expectedEnd', the peer closed the connection or 'timeout' elapsed. 'resetSeen' tells whether the connection was
// reset.
std::string SlowlyReceive(NativeHandle fd, std::chrono::milliseconds pause, std::string_view expectedEnd,
                          bool& resetSeen, std::chrono::seconds timeout = 20s) {
  resetSeen = false;
  test::setRecvTimeout(fd, std::chrono::seconds{2});
  std::string received;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!received.ends_with(expectedEnd) && std::chrono::steady_clock::now() < deadline) {
    const auto ret = test::RecvAppend(fd, received, 64UL << 10U);
    if (ret == 0 || (ret < 0 && !test::IsTransientRecvError(errno))) {
      resetSeen = ret == -1 && errno == ECONNRESET;
      break;
    }
    std::this_thread::sleep_for(pause);
  }
  return received;
}

// First non-loopback IPv4 address of the host, empty if there is none.
std::string NonLoopbackIPv4Address() {
  ifaddrs* addrs = nullptr;
  if (::getifaddrs(&addrs) != 0) {
    return {};
  }
  std::string address;
  for (const ifaddrs* ifa = addrs; ifa != nullptr && address.empty(); ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr || ifa->ifa_addr->sa_family != AF_INET || (ifa->ifa_flags & IFF_LOOPBACK) != 0 ||
        (ifa->ifa_flags & IFF_UP) == 0) {
      continue;
    }
    char buf[INET_ADDRSTRLEN];
    if (::inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(ifa->ifa_addr)->sin_addr, buf, sizeof(buf)) !=
        nullptr) {
      address = buf;
    }
  }
  ::freeifaddrs(addrs);
  return address;
}

// Restores the locked memory limit of the process on destruction.
class LockedMemoryLimitGuard {
 public:
  explicit LockedMemoryLimitGuard(rlim_t softLimit) {
    _ok = ::getrlimit(RLIMIT_MEMLOCK, &_previous) == 0;
    rlimit lowered = _previous;
    lowered.rlim_cur = softLimit;
    _ok = _ok && ::setrlimit(RLIMIT_MEMLOCK, &lowered) == 0;
  }

  LockedMemoryLimitGuard(const LockedMemoryLimitGuard&) = delete;
  LockedMemoryLimitGuard& operator=(const LockedMemoryLimitGuard&) = delete;

  ~LockedMemoryLimitGuard() { ::setrlimit(RLIMIT_MEMLOCK, &_previous); }

  [[nodiscard]] bool ok() const noexcept { return _ok; }

 private:
  rlimit _previous{};
  bool _ok{false};
};

}  // namespace

// A slow client still reading a zerocopy response beyond keepAliveTimeout receives it whole: the keep-alive timeout
// closes the connection once its sends completed, as long as they progress, instead of resetting it.
TEST(ZerocopyMode, SlowReaderBeyondKeepAliveTimeoutReceivesWholeResponse) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(1U);
    cfg.withKeepAliveTimeout(std::chrono::milliseconds{150});
  });
  // Zerocopy sends that the server send buffer takes at once: no event reports their progress until they complete.
  std::string payload(256UL << 10U, '\0');
  for (std::size_t idx = 0; idx < payload.size(); ++idx) {
    payload[idx] = static_cast<char>('a' + (idx % 26));
  }
  ts.resetRouterAndGet().setPath(http::Method::GET, "/slow-reader",
                                 [&payload](const HttpRequestView& req) { return req.makeResponse(payload); });

  const auto before = ts.server.stats();
  // The small receive buffer, emptied every 40 ms, takes about 4 times keepAliveTimeout to receive both responses: the
  // completion of the first one is activity for the connection, while the second one is still in flight.
  test::ClientConnection sock(ts.port(), 500ms, static_cast<int>(16U << 10U));
  test::sendAll(sock.fd(), "GET /slow-reader HTTP/1.1\r\nhost: h\r\n\r\nGET /slow-reader HTTP/1.1\r\nhost: h\r\n\r\n");
  bool resetSeen = false;
  std::string received;
  for (int attempt = 0; attempt < 10 && !resetSeen && test::countOccurrences(received, payload) < 2; ++attempt) {
    received += SlowlyReceive(sock.fd(), 40ms, payload, resetSeen);
  }
  EXPECT_FALSE(resetSeen);
  EXPECT_EQ(test::countOccurrences(received, payload), 2) << "received " << received.size() << " bytes";
  // The idle connection is then closed gracefully.
  EXPECT_TRUE(test::WaitForPeerClose(sock.fd(), 5000ms));
  EXPECT_EQ(ts.server.stats().zerocopyResetConnections, before.zerocopyResetConnections);
}

// Without keep-alive, a connection waiting for the completion of zerocopy sends that make no progress (the client
// reads nothing) is reset after keepAliveTimeout, instead of waiting forever.
TEST(ZerocopyMode, StalledZerocopySendsResetConnectionWithoutKeepAlive) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(1U);
    cfg.withKeepAliveMode(false);
    cfg.withKeepAliveTimeout(std::chrono::milliseconds{100});
    // Without keep-alive, the maintenance (checking the zerocopy sends) runs every pollInterval.
    cfg.withPollInterval(std::chrono::milliseconds{50});
  });
  // Small enough for the server send buffer to take it all, but far too large for the client receive buffer.
  const std::string payload(512UL << 10U, 's');
  ts.resetRouterAndGet().setPath(http::Method::GET, "/stalled-no-keep-alive",
                                 [&payload](const HttpRequestView& req) { return req.makeResponse(payload); });

  const auto before = ts.server.stats();
  test::ClientConnection sock(ts.port(), 500ms, kMinimalReceiveBufferSize);
  const NativeHandle fd = sock.fd();
  test::sendAll(fd, "GET /stalled-no-keep-alive HTTP/1.1\r\nhost: h\r\n\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  // A configuration update rebuilds the deadlines: the connection is still watched.
  ts.postConfigUpdate([](HttpServerConfig&) {});
  ExpectResetBeforeReading(ts.server, before.zerocopyResetConnections, fd, payload.size());
}

// The responses retained for zerocopy sends in flight are bounded by maxZerocopyPendingBytes: beyond it, they are
// copied (and zerocopy resumes once completions release the retained ones).
TEST(ZerocopyMode, RetainedBytesLimitFallsBackToCopy) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(1U);
    cfg.withMaxZerocopyPendingBytes(1024U);
  });
  // Larger than the batch of the responses to pipelined requests (64 KiB): each is written right away.
  const std::string payload(96UL << 10U, 'p');
  ts.resetRouterAndGet().setPath(http::Method::GET, "/retain-limit",
                                 [&payload](const HttpRequestView& req) { return req.makeResponse(payload); });

  const auto before = ts.server.stats();
  test::ClientConnection sock(ts.port());
  // Pipelined: the responses are written in a row, before any completion is polled.
  test::sendAll(sock.fd(),
                "GET /retain-limit HTTP/1.1\r\nhost: h\r\n\r\n"
                "GET /retain-limit HTTP/1.1\r\nhost: h\r\n\r\n"
                "GET /retain-limit HTTP/1.1\r\nhost: h\r\nconnection: close\r\n\r\n");
  const std::string received = test::recvUntilClosed(sock.fd());
  EXPECT_EQ(test::countOccurrences(received, payload), 3);

  const auto after = ts.server.stats();
  if (after.zerocopySends == before.zerocopySends) {
    GTEST_SKIP() << "MSG_ZEROCOPY not used here";
  }
  // The first response alone exceeds the limit, but nothing is retained yet: it uses zerocopy.
  EXPECT_EQ(after.zerocopySends - before.zerocopySends, 1U);
  EXPECT_EQ(after.zerocopyRetainLimitFallbacks - before.zerocopyRetainLimitFallbacks, 2U);
}

// A zerocopy send failing on a connection reset by the client (while its request was handled) closes it.
TEST(ZerocopyMode, ZerocopySendToResetPeerClosesConnection) {
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(1U);
  });
  const std::string payload(64UL << 10U, 'x');
  ts.resetRouterAndGet().setPath(http::Method::GET, "/reset-peer", [&payload](const HttpRequestView& req) {
    std::this_thread::sleep_for(100ms);  // the reset arrives meanwhile
    return req.makeResponse(payload);
  });

  const auto before = ts.server.stats();
  {
    test::ClientConnection sock(ts.port());
    test::sendAll(sock.fd(), "GET /reset-peer HTTP/1.1\r\nhost: h\r\n\r\n");
    ASSERT_TRUE(SetAbortiveClose(sock.fd()));
  }
  // The server keeps serving.
  std::this_thread::sleep_for(150ms);
  const auto resp = test::simpleGet(ts.port(), "/reset-peer");
  EXPECT_TRUE(resp.ends_with(payload));
  // Only the second response was sent.
  EXPECT_EQ(ts.server.stats().zerocopySends - before.zerocopySends, 1U);
}

// Stopping the server resets the connections whose zerocopy sends are in flight, and its destruction waits for the end
// of the grace period of their buffers.
TEST(ZerocopyMode, ServerStopResetsConnectionsWithZerocopySendsInFlight) {
  HttpServerConfig cfg;
  cfg.withZerocopyMode(ZerocopyMode::Enabled);
  cfg.withZerocopyMinBytes(1U);
  std::optional<test::TestServer> server(std::in_place, std::move(cfg));
  const std::string payload(256UL << 10U, 'z');
  server->resetRouterAndGet().setPath(http::Method::GET, "/stop",
                                      [&payload](const HttpRequestView& req) { return req.makeResponse(payload); });

  test::ClientConnection sock(server->port(), 500ms, kMinimalReceiveBufferSize);
  test::sendAll(sock.fd(), "GET /stop HTTP/1.1\r\nhost: h\r\n\r\n");
  std::this_thread::sleep_for(100ms);  // the response is in flight, the client reading nothing

  const auto start = std::chrono::steady_clock::now();
  server->stop();
  EXPECT_EQ(server->server.stats().zerocopyResetConnections, 1U);
  server.reset();
  // The packets handed to the network device may still read the buffers for a short while.
  EXPECT_GE(std::chrono::steady_clock::now() - start, 900ms);

  test::setRecvTimeout(sock.fd(), std::chrono::seconds{5});
  char buf[1U << 16U];
  ssize_t ret;
  while ((ret = ::recv(sock.fd(), buf, sizeof(buf), 0)) > 0) {
  }
  EXPECT_EQ(ret, -1);
  EXPECT_EQ(errno, ECONNRESET);
}

// MSG_ZEROCOPY sends failing with ENOBUFS (locked memory limit reached) fall back to copies, and the server stops
// trying for a while instead of failing each send.
TEST(ZerocopyMode, LockedMemoryExhaustionFallsBackToCopy) {
  HttpServerConfig cfg;
  cfg.withZerocopyMode(ZerocopyMode::Enabled);
  cfg.withZerocopyMinBytes(1U);
  // Declared before the server, so that it outlives the server thread that reads it.
  const std::string payload(64UL << 10U, 'm');
  test::TestServer server(std::move(cfg));
  server.resetRouterAndGet().setPath(http::Method::GET, "/memlock",
                                     [&payload](const HttpRequestView& req) { return req.makeResponse(payload); });

  // No page may be pinned anymore (unless CAP_IPC_LOCK).
  LockedMemoryLimitGuard memlockGuard(0);
  ASSERT_TRUE(memlockGuard.ok());

  test::RequestOptions opt;
  opt.target = "/memlock";
  for (int requestIdx = 0; requestIdx < 3; ++requestIdx) {
    const auto resp = test::requestOrThrow(server.port(), opt);
    EXPECT_TRUE(resp.ends_with(payload));
  }

  const auto stats = server.server.stats();
  if (stats.zerocopySends != 0) {
    GTEST_SKIP() << "pages pinned beyond RLIMIT_MEMLOCK (CAP_IPC_LOCK)";
  }
  // The first attempt failed: the next responses were copied without trying again.
  EXPECT_EQ(stats.zerocopyNoBufferSpaceFallbacks, 1U);
}

// Opportunistic mode: a peer of the same host reached through a non-loopback address (a container of the same node) is
// known once the kernel reports a copied send. The connection then stops using MSG_ZEROCOPY.
TEST(ZerocopyMode, OpportunisticModeStopsOnceTheKernelCopies) {
  const std::string localAddress = NonLoopbackIPv4Address();
  if (localAddress.empty()) {
    GTEST_SKIP() << "no non-loopback IPv4 address";
  }
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Opportunistic);
    cfg.withZerocopyMinBytes(1024U);
  });
  const std::string payload(64UL << 10U, 'o');
  ts.resetRouterAndGet().setPath(http::Method::GET, "/local-peer",
                                 [&payload](const HttpRequestView& req) { return req.makeResponse(payload); });

  BaseFd client(::socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_TRUE(client);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(ts.port());
  ASSERT_EQ(::inet_pton(AF_INET, localAddress.c_str(), &addr.sin_addr), 1);
  ASSERT_EQ(::connect(client.fd(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)), 0);

  const auto before = ts.server.stats();
  test::sendAll(client.fd(), "GET /local-peer HTTP/1.1\r\nhost: h\r\n\r\n");
  EXPECT_TRUE(test::recvWithTimeout(client.fd(), 5000ms).ends_with(payload));
  if (ts.server.stats().zerocopySends == before.zerocopySends) {
    GTEST_SKIP() << "MSG_ZEROCOPY not used here";
  }
  // Wait for the server to read the completion notification, reported copied.
  for (int attempt = 0; attempt < 500 && ts.server.stats().zerocopyCopiedSends == before.zerocopyCopiedSends;
       ++attempt) {
    std::this_thread::sleep_for(2ms);
  }
  ASSERT_GT(ts.server.stats().zerocopyCopiedSends, before.zerocopyCopiedSends);

  const auto afterFirst = ts.server.stats();
  test::sendAll(client.fd(), "GET /local-peer HTTP/1.1\r\nhost: h\r\n\r\n");
  EXPECT_TRUE(test::recvWithTimeout(client.fd(), 5000ms).ends_with(payload));
  EXPECT_EQ(ts.server.stats().zerocopySends, afterFirst.zerocopySends);
}

TEST(ZerocopyMode, StressVaryingPayloadSizes) {
  // Stress test with varying payload sizes exercising both zerocopy and regular write paths.
  // Forces zerocopy even for small payloads to stress the zerocopy completion mechanism.
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(1U);
  });

  // Payloads of different sizes to exercise edge cases
  static constexpr std::size_t sizes[]{100, 1024, 8UL * 1024, 16UL * 1024, 64UL * 1024, 256UL * 1024, 1024UL * 1024};

  for (std::size_t sz : sizes) {
    std::string payload(sz, static_cast<char>('a' + (sz % 26)));
    const std::string path = "/vary-" + std::to_string(sz);

    ts.resetRouterAndGet().setPath(http::Method::GET, path,
                                   [payload](const HttpRequestView& req) { return req.makeResponse(payload); });

    test::RequestOptions opt;
    opt.method = "GET";
    opt.target = path;
    opt.recvTimeout = std::chrono::milliseconds{5000};

    constexpr int kRepeat = 10;
    for (int rp = 0; rp < kRepeat; ++rp) {
      auto resp = test::requestOrThrow(ts.port(), opt);
      ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << "size=" << sz << " rep=" << rp;
      ASSERT_TRUE(resp.ends_with(payload)) << "data corruption at size=" << sz << " rep=" << rp;
    }
  }
}

TEST(ZerocopyMode, StressKeepAliveBackpressure) {
  // Stress test over a single keep-alive connection with many large responses.
  // This exercises the flushOutbound / output queue append paths with zerocopy.
  ts.resetConfigAndPostUpdate([](HttpServerConfig& cfg) {
    cfg.withZerocopyMode(ZerocopyMode::Enabled);
    cfg.withZerocopyMinBytes(1U);
    cfg.withKeepAliveMode(true);
    cfg.withMaxRequestsPerConnection(10000);
  });

  static constexpr std::size_t kPayloadSize = 256UL * 1024;  // 256 KB
  std::string payload;
  payload.reserve(kPayloadSize);
  for (std::size_t idx = 0; idx < kPayloadSize; ++idx) {
    payload.push_back(static_cast<char>('A' + (idx % 26)));
  }

  ts.resetRouterAndGet().setPath(http::Method::GET, "/ka-stress",
                                 [&payload](const HttpRequestView& req) { return req.makeResponse(payload); });

  // Send many requests over a single connection using keep-alive
  test::ClientConnection cnx(ts.port());
  const NativeHandle fd = cnx.fd();
  test::setRecvTimeout(fd, std::chrono::milliseconds{5000});

  constexpr int kRequests = 30;
  for (int iter = 0; iter < kRequests; ++iter) {
    const std::string reqStr = "GET /ka-stress HTTP/1.1\r\nhost: localhost\r\nconnection: keep-alive\r\n\r\n";
    test::sendAll(fd, reqStr);

    auto resp = test::recvWithTimeout(fd, std::chrono::milliseconds{5000},
                                      kPayloadSize + 119U);  // 119 = length of HTTP headers
    ASSERT_FALSE(resp.empty()) << "empty response at iteration " << iter;
    ASSERT_TRUE(resp.starts_with("HTTP/1.1 200")) << "bad status at iteration " << iter;
    ASSERT_TRUE(resp.contains(payload)) << "data corruption at iteration " << iter;
  }
}
#endif

}  // namespace aeronet
