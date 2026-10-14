#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "aeronet/http-server-config.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/test_echo_server.hpp"
#include "aeronet/test_server_http2_tls_fixture.hpp"
#include "aeronet/test_tls_http2_client.hpp"
#include "aeronet/test_util.hpp"

#ifdef AERONET_POSIX
// Its getaddrinfo override delays the resolutions.
#define AERONET_WANT_SOCKET_OVERRIDES
#include "aeronet/sys-test-support.hpp"
#endif

using namespace std::chrono_literals;
using namespace aeronet;

namespace {

void AllowConnectHost(test::TlsHttp2TestServer& testServer, std::string_view host) {
  testServer.server.postConfigUpdate([host](HttpServerConfig& cfg) {
    const std::array allowlist{host};
    cfg.withConnectAllowlist(allowlist.begin(), allowlist.end());
  });
}

// A tunnel endpoint that does not read makes the server stop taking the bytes of the other endpoint, which then wait in
// the kernel socket buffers (or in the client, without flow-control credit), instead of being buffered by the server.
constexpr std::size_t kTunnelFloodBytes = 64UL << 20U;
// The bytes a stalled tunnel holds: the server ends of the connections have socket buffers up to the tcp_rmem /
// tcp_wmem maxima.
constexpr std::size_t kTunnelBufferedBound = 48UL << 20U;
constexpr int kTestEndBufferSize = 256 * 1024;

}  // namespace

TEST(Http2ConnectTest, BasicTunneling) {
  test::TlsHttp2TestServer ts;
  AllowConnectHost(ts, "127.0.0.1");
  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto echoSrv = test::startEchoServer();
  std::string authority = "127.0.0.1:" + std::to_string(echoSrv.port);

  uint32_t streamId = client.connect(authority);
  ASSERT_GT(streamId, 0);

  std::string_view payload = "hello-http2-tunnel";
  std::span<const std::byte> data(reinterpret_cast<const std::byte*>(payload.data()), payload.size());
  ASSERT_TRUE(client.sendTunnelData(streamId, data));

  RawChars received;
  client.receiveTunnelData(received, streamId);
  std::string_view receivedStr(received.data(), received.size());
  EXPECT_EQ(receivedStr, payload);
}

TEST(Http2ConnectTest, DnsFailureReturns502) {
  test::TlsHttp2TestServer ts;
  AllowConnectHost(ts, "no-such-host.example.invalid");
  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  uint32_t streamId = client.connect("no-such-host.example.invalid:80");
  EXPECT_EQ(streamId, 0);  // connect() returns 0 on failure (non-200 status)
}

TEST(Http2ConnectTest, DefaultConfigRejectsTarget) {
  test::TlsHttp2TestServer ts;
  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  uint32_t streamId = client.connect("127.0.0.1:80");
  EXPECT_EQ(streamId, 0);
}

TEST(Http2ConnectTest, WildcardAllowlistAllowsTarget) {
  test::TlsHttp2TestServer ts;
  AllowConnectHost(ts, "*");
  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto echoSrv = test::startEchoServer();
  const std::string authority = "127.0.0.1:" + std::to_string(echoSrv.port);
  const uint32_t streamId = client.connect(authority);
  ASSERT_GT(streamId, 0);

  constexpr std::string_view payload = "wildcard-http2-connect";
  const std::span<const std::byte> data(reinterpret_cast<const std::byte*>(payload.data()), payload.size());
  ASSERT_TRUE(client.sendTunnelData(streamId, data));

  RawChars received;
  client.receiveTunnelData(received, streamId);
  EXPECT_EQ(std::string_view(received.data(), received.size()), payload);
}

TEST(Http2ConnectTest, HostNameTargetIsResolvedInBackground) {
  test::TlsHttp2TestServer ts;
  AllowConnectHost(ts, "localhost");
  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto echoSrv = test::startLocalhostEchoServer();
  const uint32_t streamId = client.connect("localhost:" + std::to_string(echoSrv.port));
  ASSERT_GT(streamId, 0);

  constexpr std::string_view payload = "resolved-http2-connect";
  const std::span<const std::byte> data(reinterpret_cast<const std::byte*>(payload.data()), payload.size());
  ASSERT_TRUE(client.sendTunnelData(streamId, data));

  RawChars received;
  client.receiveTunnelData(received, streamId);
  EXPECT_EQ(std::string_view(received.data(), received.size()), payload);
}

TEST(Http2ConnectTest, UpstreamConnectFailureResetsTheStream) {
  test::TlsHttp2TestServer ts;
  AllowConnectHost(ts, "127.0.0.1");
  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  // Nothing listens on the port of a stopped echo server anymore: the connection to the target is refused, which the
  // server only learns once the stream got its 200 response.
  uint16_t closedPort = 0;
  {
    auto echoSrv = test::startEchoServer();
    closedPort = echoSrv.port;
  }
  const uint32_t streamId = client.connect("127.0.0.1:" + std::to_string(closedPort));
  ASSERT_GT(streamId, 0);

  constexpr std::string_view payload = "never-delivered";
  const std::span<const std::byte> data(reinterpret_cast<const std::byte*>(payload.data()), payload.size());
  client.sendTunnelData(streamId, data);

  RawChars received;
  client.receiveTunnelData(received, streamId, 500ms);
  EXPECT_TRUE(received.empty());

  // The connection survives the failed tunnel.
  const uint32_t nextStreamId = client.connect("127.0.0.1:" + std::to_string(closedPort));
  EXPECT_GT(nextStreamId, streamId);
}

TEST(Http2ConnectTest, AllowlistRejectsTarget) {
  test::TlsHttp2TestServer ts;

  ts.server.postConfigUpdate([](HttpServerConfig& cfg) {
    std::array allowlist{"example.com"};
    cfg.withConnectAllowlist(allowlist.begin(), allowlist.end());
  });

  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto echoSrv = test::startEchoServer();
  std::string authority = "127.0.0.1:" + std::to_string(echoSrv.port);

  uint32_t streamId = client.connect(authority);
  EXPECT_EQ(streamId, 0);  // Should be rejected (403 Forbidden)
}

TEST(Http2ConnectTest, LargePayloadTunneling) {
  // Use large flow-control windows to minimise WINDOW_UPDATE round-trips.
  static constexpr uint32_t kLargeWindow = 1U << 20U;  // 1 MB

  test::TlsHttp2TestServer ts;
  AllowConnectHost(ts, "127.0.0.1");

  ts.server.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.http2.initialWindowSize = kLargeWindow;
    cfg.http2.connectionWindowSize = kLargeWindow * 2;
  });

  {
    Http2Config clientCfg;
    clientCfg.initialWindowSize = kLargeWindow;
    clientCfg.connectionWindowSize = kLargeWindow * 2;
    test::TlsHttp2Client client(ts.port(), clientCfg);
    ASSERT_TRUE(client.isConnected());

    auto echoSrv = test::startEchoServer();
    std::string authority = "127.0.0.1:" + std::to_string(echoSrv.port);

    uint32_t streamId = client.connect(authority);
    ASSERT_GT(streamId, 0);

#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
    std::string payload(1024UL * 1024, 'a');
#else
    std::string payload(16UL << 20U, 'a');
#endif

    std::span<const std::byte> data(reinterpret_cast<const std::byte*>(payload.data()), payload.size());

    RawChars received;
    std::size_t offset = 0;

    // Overall safety deadline. Even if the tunnel is torn down mid-transfer (e.g. the
    // upstream echo server trips its send timeout under CI load and half-closes the
    // stream), the loop below must never spin forever: once the server has sent
    // END_STREAM, receiveTunnelData() returns immediately, so a naive "wait for
    // WINDOW_UPDATE" branch would busy-loop until the CTest timeout kills the whole
    // binary. The deadline + the canReceive() check below turn any such stall into a
    // fast, explicit failure instead of a ~100 s hang.
    const auto sendDeadline = std::chrono::steady_clock::now() + std::chrono::seconds{60};

    // Cap the amount of sent-but-not-yet-echoed ("in-flight") data. If the client
    // races ahead by its full flow-control window, the echo server's socket buffers
    // fill faster than the round-trip can drain them and it trips its send timeout,
    // tearing the tunnel down. Keeping the backlog small (well under a socket buffer)
    // prevents that regardless of how heavily loaded the runner is.
    static constexpr std::size_t kMaxInFlight = 128UL << 10U;                   // 128 KB
    static constexpr int32_t kMaxSendChunk = static_cast<int32_t>(16U << 10U);  // 16 KB

    while (offset < data.size()) {
      ASSERT_LT(std::chrono::steady_clock::now(), sendDeadline) << "tunnel send loop stalled";

      auto* stream = client.connection().getStream(streamId);
      ASSERT_NE(stream, nullptr);

      // The server half-closed its side (upstream gone): no more DATA/WINDOW_UPDATE
      // frames will arrive, so stop instead of spinning on an already-complete stream.
      if (!stream->canReceive()) {
        break;
      }

      int32_t win = std::min(stream->sendWindow(), client.connection().connectionSendWindow());
      const bool inFlightFull = (offset - received.size()) >= kMaxInFlight;

      if (win <= 0 || inFlightFull) {
        // Blocked on flow control or throttled: drain incoming echo data, which also
        // processes WINDOW_UPDATE frames and reopens our send window. Note: `stream`
        // may be invalidated here, so it must be re-fetched at the top of the loop.
        client.receiveTunnelData(received, streamId, std::chrono::milliseconds{100});
        continue;
      }

      int32_t sendWin = std::min(win, kMaxSendChunk);
      std::size_t chunkSize = std::min(data.size() - offset, static_cast<std::size_t>(sendWin));
      ASSERT_TRUE(client.sendTunnelData(streamId, data.subspan(offset, chunkSize), false));
      offset += chunkSize;

      // Drain everything immediately available so our advertised receive window stays
      // open and the server keeps reading from (and writing to) the echo upstream.
      for (std::size_t before = received.size();; before = received.size()) {
        client.receiveTunnelData(received, streamId, std::chrono::milliseconds{0});
        if (received.size() == before) {
          break;
        }
      }
    }

    // Send empty END_STREAM
    ASSERT_TRUE(client.sendTunnelData(streamId, {}, true));

    while (received.size() < payload.size()) {
      const std::size_t oldSize = received.size();
      client.receiveTunnelData(received, streamId, std::chrono::milliseconds{10000});
      if (received.size() == oldSize) {
        break;
      }
    }

    std::string_view receivedSv(received.data(), received.size());

    EXPECT_EQ(receivedSv.size(), payload.size());
    EXPECT_TRUE(receivedSv.starts_with("aaaaaaaaaaaaaaaaaa"));
    EXPECT_TRUE(receivedSv.ends_with("aaaaaaaaaaaaaaaaaa"));
  }
}

namespace {

// Allows CONNECT to 127.0.0.1, with a small output limit, and a stream window of 1 MiB (fewer round trips).
void SetupTunnelBackpressureServer(test::TlsHttp2TestServer& testServer) {
  testServer.server.postConfigUpdate([](HttpServerConfig& cfg) {
    static constexpr std::array<std::string_view, 1> kAllowlist{"127.0.0.1"};
    cfg.withConnectAllowlist(kAllowlist.begin(), kAllowlist.end());
    cfg.maxOutboundBufferBytes = 256UL << 10U;
    cfg.http2.initialWindowSize = 1U << 20U;
    cfg.http2.connectionWindowSize = 4U << 20U;
  });
}

}  // namespace

TEST(Http2ConnectTest, UpstreamNotReadingStopsGrantingCreditToTheStream) {
  test::TlsHttp2TestServer ts;
  SetupTunnelBackpressureServer(ts);
  test::LoopbackPeer upstream(kTestEndBufferSize);
  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  const uint32_t streamId = client.connect("127.0.0.1:" + std::to_string(upstream.port()));
  ASSERT_GT(streamId, 0);
  const NativeHandle upstreamFd = upstream.accept();

  // Send whatever credit the server grants, until it grants no more.
  const std::string chunk(16UL << 10U, 'c');
  std::size_t sent = 0;
  auto lastProgress = std::chrono::steady_clock::now();
  while (sent < kTunnelFloodBytes && std::chrono::steady_clock::now() - lastProgress < 300ms) {
    const auto* stream = client.connection().getStream(streamId);
    ASSERT_NE(stream, nullptr);
    const int32_t window = std::min(stream->sendWindow(), client.connection().connectionSendWindow());
    if (window <= 0) {
      client.processFrames(1ms);  // WINDOW_UPDATE frames
      continue;
    }
    const auto size = std::min(chunk.size(), static_cast<std::size_t>(window));
    ASSERT_TRUE(client.sendTunnelData(streamId, std::as_bytes(std::span<const char>(chunk.data(), size))));
    sent += size;
    lastProgress = std::chrono::steady_clock::now();
  }
  log::info("Client sent {} bytes before running out of credit", sent);
  EXPECT_LT(sent, kTunnelBufferedBound);

  // Once the upstream reads, the stream gets credit again.
  EXPECT_EQ(test::ReceiveAndDiscard(upstreamFd, sent, 30s), sent);
  for (const auto deadline = std::chrono::steady_clock::now() + 5s;
       client.connection().getStream(streamId)->sendWindow() <= 0 && std::chrono::steady_clock::now() < deadline;) {
    client.processFrames(10ms);
  }
  EXPECT_GT(client.connection().getStream(streamId)->sendWindow(), 0);
}

TEST(Http2ConnectTest, ClientGrantingCreditWithoutReadingStopsReadingTheUpstream) {
  test::TlsHttp2TestServer ts;
  SetupTunnelBackpressureServer(ts);
  test::LoopbackPeer upstream(kTestEndBufferSize);
  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  const uint32_t streamId = client.connect("127.0.0.1:" + std::to_string(upstream.port()));
  ASSERT_GT(streamId, 0);
  const NativeHandle upstreamFd = upstream.accept();

  // Flow control does not bound what the server holds for a client granting credit without reading its socket: the
  // server must stop reading the upstream once its output for the client is full.
  static constexpr uint32_t kHugeCredit = 1U << 30U;
  client.connection().sendWindowUpdate(0, kHugeCredit);
  client.connection().sendWindowUpdate(streamId, kHugeCredit);
  ASSERT_TRUE(client.sendTunnelData(streamId, {}));  // flushes the WINDOW_UPDATE frames

  const std::size_t written = test::WriteUntilStalled(upstreamFd, kTunnelFloodBytes);
  log::info("Upstream wrote {} bytes before stalling", written);
  EXPECT_LT(written, kTunnelBufferedBound);

  // Once the client reads, the relay resumes until every byte reached it.
  RawChars received;
  for (const auto deadline = std::chrono::steady_clock::now() + 30s;
       received.size() < written && std::chrono::steady_clock::now() < deadline;) {
    client.receiveTunnelData(received, streamId, 1s);
  }
  EXPECT_EQ(received.size(), written);
}

#ifdef AERONET_POSIX
TEST(Http2ConnectTest, UnresolvedTargetIsAnswered504AfterKeepAliveTimeout) {
  test::TlsHttp2TestServer ts;
  ts.server.postConfigUpdate([](HttpServerConfig& cfg) {
    static constexpr std::array<std::string_view, 1> kAllowlist{"localhost"};
    cfg.withConnectAllowlist(kAllowlist.begin(), kAllowlist.end());
    cfg.withKeepAliveTimeout(300ms);
  });
  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  test::GetAddrInfoDelayGuard getAddrInfoDelayGuard(3000);
  const auto start = std::chrono::steady_clock::now();
  // A stream reset while its target is resolved is not answered (its RST_STREAM is sent with the next request).
  const uint32_t resetStreamId = client.sendConnect("localhost:9");
  ASSERT_GT(resetStreamId, 0U);
  client.connection().sendRstStream(resetStreamId, http2::ErrorCode::Cancel);
  const uint32_t streamId = client.sendConnect("localhost:9");
  ASSERT_GT(streamId, 0U);
  const auto response = client.waitAndGetResponse(streamId, 5s).value_or(test::TlsHttp2Client::Response{});
  EXPECT_EQ(response.statusCode, 504);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 2500ms);

  // The connection still serves the other streams.
  EXPECT_EQ(client.get("/").statusCode, 404);
}

TEST(Http2ConnectTest, TooManyUnresolvedTargetsAreAnswered503) {
  // A server thread lets at most 256 CONNECT requests wait for the resolution of their target (see
  // TunnelManager::kMaxPendingTunnels): the next ones are refused right away.
  static constexpr uint32_t kMaxPendingTunnels = 256;
  test::TlsHttp2TestServer ts;
  ts.server.postConfigUpdate([](HttpServerConfig& cfg) {
    static constexpr std::array<std::string_view, 1> kAllowlist{"localhost"};
    cfg.withConnectAllowlist(kAllowlist.begin(), kAllowlist.end());
    cfg.http2.maxConcurrentStreams = 2 * kMaxPendingTunnels;
  });
  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  test::GetAddrInfoDelayGuard getAddrInfoDelayGuard(2000);
  for (uint32_t idx = 0; idx < kMaxPendingTunnels; ++idx) {
    ASSERT_GT(client.sendConnect("localhost:9"), 0U);
  }
  const auto start = std::chrono::steady_clock::now();
  const uint32_t streamId = client.sendConnect("localhost:9");
  ASSERT_GT(streamId, 0U);
  const auto response = client.waitAndGetResponse(streamId, 1500ms).value_or(test::TlsHttp2Client::Response{});
  EXPECT_EQ(response.statusCode, 503);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1000ms);
}
#endif
