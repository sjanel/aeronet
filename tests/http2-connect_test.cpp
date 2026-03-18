#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/http-server-config.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/test_server_http2_tls_fixture.hpp"
#include "aeronet/test_tls_http2_client.hpp"
#include "aeronet/test_util.hpp"

using namespace std::chrono_literals;
using namespace aeronet;

TEST(Http2ConnectTest, BasicTunneling) {
  test::TlsHttp2TestServer ts;
  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto [sock, port] = test::startEchoServer();
  std::string authority = "127.0.0.1:" + std::to_string(port);

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
  test::TlsHttp2TestServer ts([](HttpServerConfig& cfg) {
    std::array<std::string_view, 1> allowlist = {"example.com"};
    cfg.withConnectAllowlist(allowlist.begin(), allowlist.end());
  });
  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto [sock, port] = test::startEchoServer();
  std::string authority = "127.0.0.1:" + std::to_string(port);

  uint32_t streamId = client.connect(authority);
  EXPECT_EQ(streamId, 0);  // Should be rejected (403 Forbidden)
}

TEST(Http2ConnectTest, LargePayloadTunneling) {
  // Use large flow-control windows to minimise WINDOW_UPDATE round-trips.
  static constexpr uint32_t kLargeWindow = 1U << 20;  // 1 MB

  test::TlsHttp2TestServer ts(nullptr, [](Http2Config& h2) {
    h2.initialWindowSize = kLargeWindow;
    h2.connectionWindowSize = kLargeWindow * 2;
  });

  {
    Http2Config clientCfg;
    clientCfg.initialWindowSize = kLargeWindow;
    clientCfg.connectionWindowSize = kLargeWindow * 2;
    test::TlsHttp2Client client(ts.port(), clientCfg);
    ASSERT_TRUE(client.isConnected());

    auto [sock, port] = test::startEchoServer();
    std::string authority = "127.0.0.1:" + std::to_string(port);

    uint32_t streamId = client.connect(authority);
    ASSERT_GT(streamId, 0);

#if defined(AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS) || defined(AERONET_WINDOWS)
    std::string payload(1024UL * 1024, 'a');
#else
    std::string payload(16UL << 20, 'a');
#endif

    std::span<const std::byte> data(reinterpret_cast<const std::byte*>(payload.data()), payload.size());

    RawChars received;
    std::size_t offset = 0;
    while (offset < data.size()) {
      auto* stream = client.connection().getStream(streamId);
      ASSERT_NE(stream, nullptr);

      int32_t streamWin = stream->sendWindow();
      int32_t connWin = client.connection().connectionSendWindow();
      int32_t win = std::min(streamWin, connWin);

      if (win <= 0) {
        // Wait for WINDOW_UPDATE
        client.receiveTunnelData(received, streamId, std::chrono::milliseconds{100});
        continue;
      }

      // Cap each send to avoid blocking in writeAll (which would deadlock if the
      // echo server can't drain because our TCP recv buffer is full).
      static constexpr int32_t kMaxSendChunk = 16 << 10;  // 16 KB
      int32_t sendWin = std::min(win, kMaxSendChunk);
      std::size_t chunkSize = std::min(data.size() - offset, static_cast<std::size_t>(sendWin));
      ASSERT_TRUE(client.sendTunnelData(streamId, data.subspan(offset, chunkSize), false));
      offset += chunkSize;

      // Non-blocking drain of incoming echo data to keep the TCP receive buffer
      // from filling up (which would prevent the server from writing).
      client.receiveTunnelData(received, streamId, std::chrono::milliseconds{0});
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
