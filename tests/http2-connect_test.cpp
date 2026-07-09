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
#include "aeronet/raw-chars.hpp"
#include "aeronet/test_echo_server.hpp"
#include "aeronet/test_server_http2_tls_fixture.hpp"
#include "aeronet/test_tls_http2_client.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

TEST(Http2ConnectTest, BasicTunneling) {
  test::TlsHttp2TestServer ts;
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
  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  uint32_t streamId = client.connect("no-such-host.example.invalid:80");
  EXPECT_EQ(streamId, 0);  // connect() returns 0 on failure (non-200 status)
}

TEST(Http2ConnectTest, AllowlistRejectsTarget) {
  test::TlsHttp2TestServer ts;

  ts.server.postConfigUpdate([](HttpServerConfig& cfg) {
    std::array<std::string_view, 1> allowlist = {"example.com"};
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
  static constexpr uint32_t kLargeWindow = 1U << 20;  // 1 MB

  test::TlsHttp2TestServer ts;

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
    std::string payload(16UL << 20, 'a');
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
    static constexpr std::size_t kMaxInFlight = 128UL << 10;  // 128 KB
    static constexpr int32_t kMaxSendChunk = 16 << 10;        // 16 KB

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
