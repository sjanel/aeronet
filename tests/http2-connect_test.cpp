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
#include "aeronet/log.hpp"
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

  auto received = client.receiveTunnelData(streamId);
  std::string receivedStr(reinterpret_cast<const char*>(received.data()), received.size());
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
  aeronet::log::set_level(aeronet::log::level::debug);
  test::TlsHttp2TestServer ts;
  test::TlsHttp2Client client(ts.port());
  ASSERT_TRUE(client.isConnected());

  auto [sock, port] = test::startEchoServer();
  std::string authority = "127.0.0.1:" + std::to_string(port);

  uint32_t streamId = client.connect(authority);
  ASSERT_GT(streamId, 0);

#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
  std::string payload(1024UL * 1024, 'a');
#else
  std::string payload(16UL << 20, 'a');
#endif

  std::span<const std::byte> data(reinterpret_cast<const std::byte*>(payload.data()), payload.size());

  std::string receivedStr;
  std::size_t offset = 0;
  while (offset < data.size()) {
    auto* stream = client.connection().getStream(streamId);
    ASSERT_NE(stream, nullptr);

    int32_t streamWin = stream->sendWindow();
    int32_t connWin = client.connection().connectionSendWindow();
    int32_t win = std::min(streamWin, connWin);

    if (win <= 0) {
      // Wait for WINDOW_UPDATE
      auto received = client.receiveTunnelData(streamId, std::chrono::milliseconds{100});
      receivedStr.append(reinterpret_cast<const char*>(received.data()), received.size());
      continue;
    }

    std::size_t chunkSize = std::min({data.size() - offset, static_cast<std::size_t>(win)});
    ASSERT_TRUE(client.sendTunnelData(streamId, data.subspan(offset, chunkSize), false));
    offset += chunkSize;

    // Also receive data to prevent the server from blocking
    while (true) {
      auto received = client.receiveTunnelData(streamId, std::chrono::milliseconds{10});
      if (received.empty()) {
        break;
      }
      receivedStr.append(reinterpret_cast<const char*>(received.data()), received.size());
    }
  }

  // Send empty END_STREAM
  ASSERT_TRUE(client.sendTunnelData(streamId, {}, true));

  while (receivedStr.size() < payload.size()) {
    auto received = client.receiveTunnelData(streamId, std::chrono::milliseconds{10000});
    if (received.empty()) {
      break;
    }
    receivedStr.append(reinterpret_cast<const char*>(received.data()), received.size());
  }

  EXPECT_EQ(receivedStr.size(), payload.size());
  EXPECT_TRUE(receivedStr.starts_with("aaaaaaaaaaaaaaaaaa"));
  EXPECT_TRUE(receivedStr.ends_with("aaaaaaaaaaaaaaaaaa"));
}
