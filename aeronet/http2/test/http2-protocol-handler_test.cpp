#include "aeronet/http2-protocol-handler.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/connection-state.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/path-handler-entry.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/request-task.hpp"
#endif
#include "aeronet/router.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/tunnel-bridge.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::http2 {
namespace {

RawChars tmpBuffer;
tracing::TelemetryContext telemetry;

struct HeaderEvent {
  uint32_t streamId{0};
  bool endStream{false};
  vector<std::pair<std::string, std::string>> headers;
};

struct DataEvent {
  uint32_t streamId{0};
  bool endStream{false};
  std::string data;
};

[[nodiscard]] bool HasHeader(const HeaderEvent& ev, std::string_view name, std::string_view value) {
  return std::ranges::any_of(ev.headers, [&](const auto& kv) { return kv.first == name && kv.second == value; });
}

[[nodiscard]] std::string GetHeaderValue(const HeaderEvent& ev, std::string_view name) {
  for (const auto& [key, value] : ev.headers) {
    if (key == name) {
      return value;
    }
  }
  return {};
}
}  // namespace

/// Test mock for ITunnelBridge that delegates to std::function members.
class MockTunnelBridge final : public ITunnelBridge {
 public:
  std::function<int(uint32_t, std::string_view, std::string_view)> onSetup;
  std::function<void(int, std::span<const std::byte>)> onWrite;
  std::function<void(int)> onShutdownWrite;
  std::function<void(int)> onClose;
  std::function<void(int)> onWindowUpdate;

  int setupTunnel(uint32_t streamId, std::string_view host, std::string_view port) override {
    return onSetup ? onSetup(streamId, host, port) : -1;
  }

  void writeTunnel(int upstreamFd, std::span<const std::byte> data) override {
    if (onWrite) {
      onWrite(upstreamFd, data);
    }
  }

  void shutdownTunnelWrite(int upstreamFd) override {
    if (onShutdownWrite) {
      onShutdownWrite(upstreamFd);
    }
  }

  void closeTunnel(int upstreamFd) override {
    if (onClose) {
      onClose(upstreamFd);
    }
  }

  void onTunnelWindowUpdate(int upstreamFd) override {
    if (onWindowUpdate) {
      onWindowUpdate(upstreamFd);
    }
  }
};

class Http2ProtocolLoopback {
 public:
  explicit Http2ProtocolLoopback(Router& router)
      : compressionState(serverConfig.compression),
        handler(serverCfg, router, serverConfig, compressionState, decompressionState, telemetry, tmpBuffer),
        client(clientCfg, false) {
    client.setOnHeadersDecoded([this](uint32_t streamId, const HeadersViewMap& headers, bool endStream) {
      HeaderEvent ev;
      ev.streamId = streamId;
      ev.endStream = endStream;
      for (const auto& [name, value] : headers) {
        ev.headers.emplace_back(name, value);
      }
      clientHeaders.push_back(std::move(ev));
    });

    client.setOnData([this](uint32_t streamId, std::span<const std::byte> data, bool endStream) {
      DataEvent ev;
      ev.streamId = streamId;
      ev.endStream = endStream;
      ev.data.assign(reinterpret_cast<const char*>(data.data()), data.size());
      clientData.push_back(std::move(ev));
    });

    client.setOnStreamReset(
        [this](uint32_t streamId, ErrorCode errorCode) { streamResets.emplace_back(streamId, errorCode); });
  }

  void connect() {
    client.sendClientPreface();
    pumpClientToServer();
    pumpServerToClient();
    pumpClientToServer();
    pumpServerToClient();

    ASSERT_EQ(handler.connection().state(), ConnectionState::Open);
    ASSERT_EQ(client.state(), ConnectionState::Open);
  }

  void pumpClientToServer(std::size_t maxChunks = 128) {
    std::size_t chunks = 0;
    while (client.hasPendingOutput()) {
      ++chunks;
      if (chunks > maxChunks) {
        ADD_FAILURE() << "pumpClientToServer exceeded maxChunks";
        return;
      }
      auto out = client.getPendingOutput();
      vector<std::byte> outCopy;
      outCopy.reserve(static_cast<decltype(outCopy)::size_type>(out.size()));
      std::ranges::copy(out, std::back_inserter(outCopy));
      feedHandler(outCopy);
      client.onOutputWritten(out.size());
    }
  }

  void pumpServerToClient(std::size_t maxChunks = 128) {
    std::size_t chunks = 0;
    while (handler.hasPendingOutput()) {
      ++chunks;
      if (chunks > maxChunks) {
        ADD_FAILURE() << "pumpServerToClient exceeded maxChunks";
        return;
      }
      auto out = handler.getPendingOutput();
      vector<std::byte> outCopy;
      outCopy.reserve(static_cast<decltype(outCopy)::size_type>(out.size()));
      std::ranges::copy(out, std::back_inserter(outCopy));
      feedConn(client, outCopy);
      handler.onOutputWritten(out.size());
    }
  }

  void feedHandler(std::span<const std::byte> bytes) {
    std::size_t safetyIters = 0;
    while (!bytes.empty()) {
      ++safetyIters;
      if (safetyIters > 64U) {
        ADD_FAILURE() << "feedHandler got stuck";
        return;
      }

      auto res = handler.processInput(bytes, state);
      if (res.bytesConsumed == 0) {
        ADD_FAILURE() << "No progress feeding handler";
        return;
      }
      bytes = bytes.subspan(res.bytesConsumed);
    }
  }

  static void feedConn(Http2Connection& conn, std::span<const std::byte> bytes) {
    std::size_t safetyIters = 0;
    while (!bytes.empty()) {
      ++safetyIters;
      if (safetyIters > 64U) {
        ADD_FAILURE() << "feedConn got stuck";
        return;
      }

      const auto prevState = conn.state();
      auto res = conn.processInput(bytes);

      if (res.action == Http2Connection::ProcessResult::Action::Error ||
          res.action == Http2Connection::ProcessResult::Action::Closed ||
          res.action == Http2Connection::ProcessResult::Action::GoAway) {
        if (res.bytesConsumed > 0) {
          bytes = bytes.subspan(res.bytesConsumed);
        }
        return;
      }

      if (res.bytesConsumed > 0) {
        bytes = bytes.subspan(res.bytesConsumed);
        continue;
      }

      if (conn.state() != prevState) {
        continue;
      }

      ADD_FAILURE() << "No progress feeding connection";
      return;
    }
  }

  Http2Config serverCfg;
  Http2Config clientCfg;

  HttpServerConfig serverConfig;
  internal::ResponseCompressionState compressionState;
  internal::RequestDecompressionState decompressionState;

  Http2ProtocolHandler handler;
  Http2Connection client;
  ::aeronet::ConnectionState state;

  vector<HeaderEvent> clientHeaders;
  vector<DataEvent> clientData;
  vector<std::pair<uint32_t, ErrorCode>> streamResets;
};

TEST(Http2ProtocolHandler, Creation) {
  Http2Config config;
  Router router;
  bool handlerCalled = false;

  HttpServerConfig serverConfig;
  internal::ResponseCompressionState compressionState(serverConfig.compression);
  internal::RequestDecompressionState decompressionState;

  router.setDefault([&handlerCalled](const HttpRequest& /*req*/) {
    handlerCalled = true;
    return HttpResponse(200);
  });

  auto handler = CreateHttp2ProtocolHandler(config, router, serverConfig, compressionState, decompressionState,
                                            telemetry, tmpBuffer);

  ASSERT_NE(handler, nullptr);
  EXPECT_EQ(handler->type(), ProtocolType::Http2);
  EXPECT_FALSE(handlerCalled);
}

TEST(Http2ProtocolHandler, HasNoPendingOutputInitially) {
  Http2Config config;
  Router router;
  HttpServerConfig serverConfig;
  internal::ResponseCompressionState compressionState(serverConfig.compression);
  internal::RequestDecompressionState decompressionState;
  auto handler = CreateHttp2ProtocolHandler(config, router, serverConfig, compressionState, decompressionState,
                                            telemetry, tmpBuffer);

  EXPECT_FALSE(handler->hasPendingOutput());
}

TEST(Http2ProtocolHandler, ConnectionPreface) {
  Http2Config config;
  Router router;
  HttpServerConfig serverConfig;
  internal::ResponseCompressionState compressionState(serverConfig.compression);
  internal::RequestDecompressionState decompressionState;
  auto handler = CreateHttp2ProtocolHandler(config, router, serverConfig, compressionState, decompressionState,
                                            telemetry, tmpBuffer);

  EXPECT_FALSE(handler->hasPendingOutput());
}

TEST(Http2ProtocolHandler, InitiateClose) {
  Http2Config config;
  Router router;
  HttpServerConfig serverConfig;
  internal::ResponseCompressionState compressionState(serverConfig.compression);
  internal::RequestDecompressionState decompressionState;
  auto handler = CreateHttp2ProtocolHandler(config, router, serverConfig, compressionState, decompressionState,
                                            telemetry, tmpBuffer);

  if (handler->hasPendingOutput()) {
    auto output = handler->getPendingOutput();
    handler->onOutputWritten(output.size());
  }

  handler->initiateClose();

  EXPECT_TRUE(handler->hasPendingOutput());
  auto output = handler->getPendingOutput();
  ASSERT_GE(output.size(), FrameHeader::kSize);
  EXPECT_EQ(ParseFrameHeader(output).type, FrameType::GoAway);
}

TEST(CreateHttp2ProtocolHandler, ReturnsValidHandler) {
  Http2Config config;
  config.maxConcurrentStreams = 200;
  config.initialWindowSize = 32768;

  HttpServerConfig serverConfig;
  internal::ResponseCompressionState compressionState(serverConfig.compression);

  Router router;
  router.setDefault([](const HttpRequest& req) { return HttpResponse("Hello from " + std::string(req.path())); });

  internal::RequestDecompressionState decompressionState;
  auto handler = CreateHttp2ProtocolHandler(config, router, serverConfig, compressionState, decompressionState,
                                            telemetry, tmpBuffer);

  ASSERT_NE(handler, nullptr);
  EXPECT_EQ(handler->type(), ProtocolType::Http2);
}

TEST(CreateHttp2ProtocolHandler, SendServerPrefaceForTlsQueuesSettingsImmediately) {
  Http2Config config;
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  HttpServerConfig serverConfig;
  internal::ResponseCompressionState compressionState(serverConfig.compression);

  internal::RequestDecompressionState decompressionState;
  auto handlerBase = CreateHttp2ProtocolHandler(config, router, serverConfig, compressionState, decompressionState,
                                                telemetry, tmpBuffer, true);
  auto* handler = dynamic_cast<Http2ProtocolHandler*>(handlerBase.get());
  ASSERT_NE(handler, nullptr);

  ASSERT_TRUE(handler->hasPendingOutput());
  const auto out = handler->getPendingOutput();
  ASSERT_GE(out.size(), FrameHeader::kSize);
  EXPECT_EQ(ParseFrameHeader(out).type, FrameType::Settings);
}

TEST(Http2ProtocolHandler, ProcessInputInvalidPrefaceRequestsImmediateClose) {
  Http2Config config;
  Router router;
  HttpServerConfig serverConfig;
  internal::ResponseCompressionState compressionState(serverConfig.compression);
  internal::RequestDecompressionState decompressionState;
  Http2ProtocolHandler handler(config, router, serverConfig, compressionState, decompressionState, telemetry,
                               tmpBuffer);
  ::aeronet::ConnectionState st;

  std::array<std::byte, 24> invalidPreface{};
  auto res = handler.processInput(invalidPreface, st);
  EXPECT_EQ(res.action, ProtocolProcessResult::Action::CloseImmediate);

  // After a protocol error, the underlying connection transitions to Closed;
  // further input should map to Close.
  std::array<std::byte, 1> more{};
  res = handler.processInput(more, st);
  EXPECT_EQ(res.action, ProtocolProcessResult::Action::Close);
}

TEST(Http2ProtocolHandler, MoveConstructAndAssignAreNoexceptAndUsable) {
  Http2Config config;
  Router router;
  HttpServerConfig serverConfig;
  internal::ResponseCompressionState compressionState(serverConfig.compression);
  internal::RequestDecompressionState decompressionState;
  Http2ProtocolHandler original(config, router, serverConfig, compressionState, decompressionState, telemetry,
                                tmpBuffer);

  static_assert(noexcept(Http2ProtocolHandler(std::declval<Http2ProtocolHandler&&>())));
  static_assert(noexcept(std::declval<Http2ProtocolHandler&>() = std::declval<Http2ProtocolHandler&&>()));

  Http2ProtocolHandler moved(std::move(original));
  EXPECT_FALSE(moved.hasPendingOutput());

  Http2ProtocolHandler assigned(config, router, serverConfig, compressionState, decompressionState, telemetry,
                                tmpBuffer);
  assigned = std::move(moved);
  EXPECT_FALSE(assigned.hasPendingOutput());
}

TEST(Http2ProtocolHandler, SimpleGetWithBodyProducesHeadersAndData) {
  Router router;
  router.setPath(http::Method::GET, "/hello", [](const HttpRequest&) { return HttpResponse(200, "abc"); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs1;
  hdrs1.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs1.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs1.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs1.append(MakeHttp1HeaderLine(":path", "/hello"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCodeOK, HeadersView(hdrs1), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
  EXPECT_FALSE(loop.clientHeaders[0].endStream);

  ASSERT_FALSE(loop.clientData.empty());
  EXPECT_EQ(loop.clientData[0].data, "abc");
  EXPECT_TRUE(loop.clientData[0].endStream);
}

TEST(Http2ProtocolHandler, ConnectMalformedTargetReturns400) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  // Install a tunnel bridge that should never be called.
  bool setupCalled = false;
  MockTunnelBridge bridge;
  bridge.onSetup = [&](uint32_t, std::string_view, std::string_view) -> int {
    setupCalled = true;
    return -1;
  };
  loop.handler.setTunnelBridge(&bridge);

  // Target without port separator → 400.
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "example.com"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "400");
  EXPECT_FALSE(setupCalled);
}

TEST(Http2ProtocolHandler, ConnectMalformedTargetEmptyPort) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> int { return -1; };
  loop.handler.setTunnelBridge(&bridge);

  // Target with empty port → 400.
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "example.com:"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "400");
}

TEST(Http2ProtocolHandler, ConnectMalformedTargetEmptyHost) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> int { return -1; };
  loop.handler.setTunnelBridge(&bridge);

  // Target with empty host → 400.
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", ":443"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "400");
}

TEST(Http2ProtocolHandler, ConnectAllowlistBlocksUnlistedTarget) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  std::array<std::string_view, 1> allowedHosts = {"allowed.example.com"};
  loop.serverConfig.withConnectAllowlist(allowedHosts.begin(), allowedHosts.end());
  loop.connect();

  bool setupCalled = false;
  MockTunnelBridge bridge;
  bridge.onSetup = [&](uint32_t, std::string_view, std::string_view) -> int {
    setupCalled = true;
    return 42;
  };
  loop.handler.setTunnelBridge(&bridge);

  // Target not in allowlist → 403.
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "blocked.example.com:443"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "403");
  EXPECT_FALSE(setupCalled);
}

TEST(Http2ProtocolHandler, ConnectSetupFailureReturns502) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  // Setup returns -1 → upstream connect failed → 502.
  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> int { return -1; };
  loop.handler.setTunnelBridge(&bridge);

  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "example.com:443"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "502");
}

TEST(Http2ProtocolHandler, ConnectTunnelEstablished) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  int capturedStreamId = -1;
  std::string capturedHost;
  std::string capturedPort;
  constexpr int kFakeUpstreamFd = 42;

  MockTunnelBridge bridge;
  bridge.onSetup = [&](uint32_t streamId, std::string_view host, std::string_view port) -> int {
    capturedStreamId = static_cast<int>(streamId);
    capturedHost = host;
    capturedPort = port;
    return kFakeUpstreamFd;
  };
  loop.handler.setTunnelBridge(&bridge);

  // Send CONNECT without END_STREAM — the client wants to keep sending data.
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "example.com:443"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), false);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  // Verify the setup callback was called with the correct parameters.
  EXPECT_EQ(capturedStreamId, 1);
  EXPECT_EQ(capturedHost, "example.com");
  EXPECT_EQ(capturedPort, "443");

  // Verify the tunnel is now active.
  EXPECT_TRUE(loop.handler.isTunnelStream(1));

  // Verify the handler sent a 200 response without END_STREAM.
  ASSERT_FALSE(loop.clientHeaders.empty());
  const auto& resp = loop.clientHeaders.back();
  EXPECT_EQ(GetHeaderValue(resp, ":status"), "200");
  EXPECT_FALSE(resp.endStream);
}

TEST(Http2ProtocolHandler, ConnectTunnelForwardsDataClientToUpstream) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  constexpr int kFakeUpstreamFd = 42;
  std::string writtenData;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> int { return kFakeUpstreamFd; };
  bridge.onWrite = [&](int upstreamFd, std::span<const std::byte> data) {
    EXPECT_EQ(upstreamFd, kFakeUpstreamFd);
    writtenData.append(reinterpret_cast<const char*>(data.data()), data.size());
  };
  loop.handler.setTunnelBridge(&bridge);

  // Establish the tunnel.
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "example.com:443"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), false), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "200");

  // Send DATA from client → should be forwarded to the upstream via the write callback.
  const std::string_view tunnelPayload = "Hello, tunnel!";
  ASSERT_EQ(
      loop.client.sendData(1, std::as_bytes(std::span<const char>(tunnelPayload.data(), tunnelPayload.size())), false),
      ErrorCode::NoError);

  loop.pumpClientToServer();

  EXPECT_EQ(writtenData, "Hello, tunnel!");
}

TEST(Http2ProtocolHandler, ConnectTunnelInjectsDataUpstreamToClient) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  constexpr int kFakeUpstreamFd = 42;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> int { return kFakeUpstreamFd; };
  loop.handler.setTunnelBridge(&bridge);

  // Establish the tunnel on stream 1.
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "example.com:443"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), false), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "200");

  // Inject data from upstream → should appear as DATA frames to the client.
  const std::string_view upstreamPayload = "Response from upstream";
  auto err = loop.handler.injectTunnelData(
      1, std::as_bytes(std::span<const char>(upstreamPayload.data(), upstreamPayload.size())));
  EXPECT_EQ(err, ErrorCode::NoError);

  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientData.empty());
  EXPECT_EQ(loop.clientData.back().streamId, 1U);
  EXPECT_EQ(loop.clientData.back().data, "Response from upstream");
  EXPECT_FALSE(loop.clientData.back().endStream);
}

TEST(Http2ProtocolHandler, ConnectTunnelClientEndStreamHalfClosesTunnel) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  constexpr int kFakeUpstreamFd = 42;

  bool shutdownWriteCalled = false;
  int shutdownWriteFd = -1;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> int { return kFakeUpstreamFd; };
  bridge.onShutdownWrite = [&](int upstreamFd) {
    shutdownWriteCalled = true;
    shutdownWriteFd = upstreamFd;
  };
  loop.handler.setTunnelBridge(&bridge);

  // Establish tunnel.
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "example.com:443"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), false), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();
  EXPECT_TRUE(loop.handler.isTunnelStream(1));

  // Send DATA with END_STREAM → client closes their end of the tunnel.
  ASSERT_EQ(loop.client.sendData(1, {}, true), ErrorCode::NoError);

  loop.pumpClientToServer();

  // The tunnel should be half-closed and the shutdownWrite callback should have been called.
  EXPECT_TRUE(loop.handler.isTunnelStream(1));
  EXPECT_TRUE(shutdownWriteCalled);
  EXPECT_EQ(shutdownWriteFd, kFakeUpstreamFd);
}

TEST(Http2ProtocolHandler, ConnectTunnelClosedByUpstreamSendsEndStream) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  constexpr int kFakeUpstreamFd = 42;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> int { return kFakeUpstreamFd; };
  loop.handler.setTunnelBridge(&bridge);

  // Establish tunnel.
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "example.com:443"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), false), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();
  EXPECT_TRUE(loop.handler.isTunnelStream(1));

  // Upstream fd closes → handler sends empty DATA with END_STREAM.
  loop.handler.closeTunnelByUpstreamFd(kFakeUpstreamFd);
  EXPECT_FALSE(loop.handler.isTunnelStream(1));

  loop.pumpServerToClient();

  // Client should receive a DATA frame with END_STREAM.
  ASSERT_FALSE(loop.clientData.empty());
  EXPECT_EQ(loop.clientData.back().streamId, 1U);
  EXPECT_TRUE(loop.clientData.back().endStream);
}

TEST(Http2ProtocolHandler, ConnectTunnelConnectFailedSendsRstStream) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  constexpr int kFakeUpstreamFd = 42;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> int { return kFakeUpstreamFd; };
  loop.handler.setTunnelBridge(&bridge);

  // Establish tunnel.
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "example.com:443"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), false), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();
  EXPECT_TRUE(loop.handler.isTunnelStream(1));

  // Async connect failed → handler sends RST_STREAM with CONNECT_ERROR.
  loop.handler.tunnelConnectFailed(1);
  EXPECT_FALSE(loop.handler.isTunnelStream(1));

  loop.pumpServerToClient();

  // Client should receive RST_STREAM with CONNECT_ERROR.
  ASSERT_FALSE(loop.streamResets.empty());
  EXPECT_EQ(loop.streamResets.back().first, 1U);
  EXPECT_EQ(loop.streamResets.back().second, ErrorCode::ConnectError);
}

TEST(Http2ProtocolHandler, ConnectTunnelStreamResetCleanupsTunnel) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  constexpr int kFakeUpstreamFd = 42;
  bool closeCalled = false;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> int { return kFakeUpstreamFd; };
  bridge.onClose = [&](int) { closeCalled = true; };
  loop.handler.setTunnelBridge(&bridge);

  // Establish tunnel.
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "example.com:443"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), false), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();
  EXPECT_TRUE(loop.handler.isTunnelStream(1));

  // Client sends RST_STREAM on the tunnel stream.
  loop.client.sendRstStream(1, ErrorCode::Cancel);

  loop.pumpClientToServer();

  // Tunnel should be cleaned up.
  EXPECT_FALSE(loop.handler.isTunnelStream(1));
  EXPECT_TRUE(closeCalled);
}

TEST(Http2ProtocolHandler, ConnectTunnelBidirectionalDataFlow) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  constexpr int kFakeUpstreamFd = 42;
  std::string allWrittenData;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> int { return kFakeUpstreamFd; };
  bridge.onWrite = [&](int, std::span<const std::byte> data) {
    allWrittenData.append(reinterpret_cast<const char*>(data.data()), data.size());
  };
  loop.handler.setTunnelBridge(&bridge);

  // Establish tunnel.
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "example.com:443"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), false), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  // Client → Upstream: multiple data chunks.
  for (int idx = 0; idx < 5; ++idx) {
    std::string chunk = "chunk-" + std::to_string(idx);
    ASSERT_EQ(loop.client.sendData(1, std::as_bytes(std::span<const char>(chunk.data(), chunk.size())), false),
              ErrorCode::NoError);
    loop.pumpClientToServer();
  }

  EXPECT_EQ(allWrittenData, "chunk-0chunk-1chunk-2chunk-3chunk-4");

  // Upstream → Client: multiple data injections.
  for (int idx = 0; idx < 3; ++idx) {
    std::string payload = "reply-" + std::to_string(idx);
    auto err = loop.handler.injectTunnelData(1, std::as_bytes(std::span<const char>(payload.data(), payload.size())));
    EXPECT_EQ(err, ErrorCode::NoError);
  }
  loop.pumpServerToClient();

  // Verify all upstream→client data was received.
  ASSERT_GE(loop.clientData.size(), 3U);
  std::string allReceivedData;
  for (const auto& ev : loop.clientData) {
    allReceivedData += ev.data;
  }
  EXPECT_EQ(allReceivedData, "reply-0reply-1reply-2");
}

TEST(Http2ProtocolHandler, ConnectTunnelLargeDataTransfer) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  constexpr int kFakeUpstreamFd = 42;
  std::string writtenData;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> int { return kFakeUpstreamFd; };
  bridge.onWrite = [&](int, std::span<const std::byte> data) {
    writtenData.append(reinterpret_cast<const char*>(data.data()), data.size());
  };
  loop.handler.setTunnelBridge(&bridge);

  // Establish tunnel.
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "example.com:443"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), false), ErrorCode::NoError);
  loop.pumpClientToServer();
  loop.pumpServerToClient();

  // Send a large data payload (exceeding typical flow control window).
  // The default initial window size is 65535 bytes. We'll send data in chunks
  // and pump regularly to handle flow control.
  std::string largePayload(16384, 'X');
  for (int idx = 0; idx < 4; ++idx) {
    ASSERT_EQ(
        loop.client.sendData(1, std::as_bytes(std::span<const char>(largePayload.data(), largePayload.size())), false),
        ErrorCode::NoError);
    loop.pumpClientToServer();
    loop.pumpServerToClient();  // Allow WINDOW_UPDATE frames to flow back.
    loop.pumpClientToServer();  // Process any buffered data after window update.
  }

  EXPECT_EQ(writtenData.size(), 65536U);
}

TEST(Http2ProtocolHandler, ConnectTunnelOnTransportClosingCleansUp) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  std::vector<int> closedFds;
  constexpr int kFakeUpstreamFd1 = 42;
  constexpr int kFakeUpstreamFd2 = 43;
  int nextFd = kFakeUpstreamFd1;

  MockTunnelBridge bridge;
  bridge.onSetup = [&](uint32_t, std::string_view, std::string_view) -> int {
    int fd = nextFd++;
    return fd;
  };
  bridge.onClose = [&](int fd) { closedFds.push_back(fd); };
  loop.handler.setTunnelBridge(&bridge);

  // Establish two tunnels on different streams.
  RawChars conn1;
  conn1.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn1.append(MakeHttp1HeaderLine(":authority", "example.com:443"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn1), false), ErrorCode::NoError);
  loop.pumpClientToServer();
  loop.pumpServerToClient();

  RawChars conn3;
  conn3.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn3.append(MakeHttp1HeaderLine(":authority", "other.com:8080"));
  ASSERT_EQ(loop.client.sendHeaders(3, http::StatusCode{}, HeadersView(conn3), false), ErrorCode::NoError);
  loop.pumpClientToServer();
  loop.pumpServerToClient();

  EXPECT_TRUE(loop.handler.isTunnelStream(1));
  EXPECT_TRUE(loop.handler.isTunnelStream(3));

  // Simulate transport closing — all tunnels should be cleaned up.
  loop.handler.onTransportClosing();

  EXPECT_FALSE(loop.handler.isTunnelStream(1));
  EXPECT_FALSE(loop.handler.isTunnelStream(3));
  EXPECT_EQ(closedFds.size(), 2U);
  // Both fds should have been closed (order may vary with flat hash map iteration).
  EXPECT_TRUE(std::ranges::find(closedFds, kFakeUpstreamFd1) != closedFds.end());
  EXPECT_TRUE(std::ranges::find(closedFds, kFakeUpstreamFd2) != closedFds.end());
}

TEST(Http2ProtocolHandler, ConnectTunnelDrainUpstreamFds) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  constexpr int kFakeUpstreamFd1 = 42;
  constexpr int kFakeUpstreamFd2 = 43;
  int nextFd = kFakeUpstreamFd1;

  MockTunnelBridge bridge;
  bridge.onSetup = [&](uint32_t, std::string_view, std::string_view) -> int { return nextFd++; };
  loop.handler.setTunnelBridge(&bridge);

  // Establish two tunnels.
  RawChars conn1;
  conn1.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn1.append(MakeHttp1HeaderLine(":authority", "a.com:80"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn1), false), ErrorCode::NoError);
  loop.pumpClientToServer();
  loop.pumpServerToClient();

  RawChars conn3;
  conn3.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn3.append(MakeHttp1HeaderLine(":authority", "b.com:80"));
  ASSERT_EQ(loop.client.sendHeaders(3, http::StatusCode{}, HeadersView(conn3), false), ErrorCode::NoError);
  loop.pumpClientToServer();
  loop.pumpServerToClient();

  EXPECT_TRUE(loop.handler.isTunnelStream(1));
  EXPECT_TRUE(loop.handler.isTunnelStream(3));

  // drainTunnelUpstreamFds should return all fds and clear internal state.
  auto fds = loop.handler.drainTunnelUpstreamFds();

  EXPECT_EQ(fds.size(), 2U);
  EXPECT_TRUE(fds.contains(kFakeUpstreamFd1));
  EXPECT_TRUE(fds.contains(kFakeUpstreamFd2));

  // After drain, no tunnel streams should remain.
  EXPECT_FALSE(loop.handler.isTunnelStream(1));
  EXPECT_FALSE(loop.handler.isTunnelStream(3));
}

TEST(Http2ProtocolHandler, ConnectTunnelCoexistsWithNormalRequests) {
  Router router;
  router.setPath(http::Method::GET, "/hello", [](const HttpRequest&) { return HttpResponse(200, "world"); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  constexpr int kFakeUpstreamFd = 42;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> int { return kFakeUpstreamFd; };
  loop.handler.setTunnelBridge(&bridge);

  // Establish a tunnel on stream 1.
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "example.com:443"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), false), ErrorCode::NoError);
  loop.pumpClientToServer();
  loop.pumpServerToClient();

  EXPECT_TRUE(loop.handler.isTunnelStream(1));
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "200");

  // Send a normal GET request on stream 3 (coexists with the tunnel on stream 1).
  RawChars getHdrs;
  getHdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  getHdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  getHdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  getHdrs.append(MakeHttp1HeaderLine(":path", "/hello"));
  ASSERT_EQ(loop.client.sendHeaders(3, http::StatusCodeOK, HeadersView(getHdrs), true), ErrorCode::NoError);
  loop.pumpClientToServer();
  loop.pumpServerToClient();

  // The GET response should be on stream 3 with body "world".
  bool foundGetResp = false;
  for (const auto& hdr : loop.clientHeaders) {
    if (hdr.streamId == 3) {
      EXPECT_EQ(GetHeaderValue(hdr, ":status"), "200");
      foundGetResp = true;
    }
  }
  EXPECT_TRUE(foundGetResp);

  bool foundBody = false;
  for (const auto& de : loop.clientData) {
    if (de.streamId == 3) {
      EXPECT_EQ(de.data, "world");
      foundBody = true;
    }
  }
  EXPECT_TRUE(foundBody);

  // The tunnel on stream 1 should still be active.
  EXPECT_TRUE(loop.handler.isTunnelStream(1));
}

TEST(Http2ProtocolHandler, HttpRequestHttp2FieldsSetCorrectly) {
  Router router;
  router.setPath(http::Method::GET, "/hello", [](const HttpRequest& req) {
    std::string body = "Handler called\n";
    body += "isHttp2: " + std::string(req.isHttp2() ? "true" : "false") + "\n";
    body += "streamId: " + std::to_string(req.streamId()) + "\n";
    body += "scheme: " + std::string(req.scheme()) + "\n";
    body += "authority: " + std::string(req.authority()) + "\n";
    return HttpResponse(body);
  });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs2;
  hdrs2.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs2.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs2.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs2.append(MakeHttp1HeaderLine(":path", "/hello"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCodeOK, HeadersView(hdrs2), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
  EXPECT_FALSE(loop.clientHeaders[0].endStream);

  ASSERT_FALSE(loop.clientData.empty());
  EXPECT_TRUE(loop.clientData[0].endStream);
  EXPECT_EQ(loop.clientData[0].data,
            "Handler called\nisHttp2: true\nstreamId: 1\nscheme: https\nauthority: example.com\n");
}

TEST(Http2ProtocolHandler, ResponseWithTrailersEndsOnTrailerHeaders) {
  Router router;
  router.setPath(http::Method::GET, "/trailers",
                 [](const HttpRequest&) { return HttpResponse(200, "abc").trailerAddLine("x-check", "ok"); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs5;
  hdrs5.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs5.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs5.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs5.append(MakeHttp1HeaderLine(":path", "/trailers"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCodeOK, HeadersView(hdrs5), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_GE(loop.clientHeaders.size(), 2U);
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
  EXPECT_FALSE(loop.clientHeaders[0].endStream);

  ASSERT_FALSE(loop.clientData.empty());
  EXPECT_EQ(loop.clientData[0].data, "abc");
  EXPECT_FALSE(loop.clientData[0].endStream);

  EXPECT_TRUE(loop.clientHeaders[1].endStream);
  EXPECT_TRUE(HasHeader(loop.clientHeaders[1], "x-check", "ok"));
  EXPECT_FALSE(HasHeader(loop.clientHeaders[1], ":status", "200"));
}

TEST(Http2ProtocolHandler, ResponseWithTrailersButNoBodyEndsOnTrailerHeadersWithoutData) {
  Router router;
  router.setPath(http::Method::GET, "/trailers-nobody",
                 [](const HttpRequest&) { return HttpResponse(200).trailerAddLine("x-check", "ok"); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs6;
  hdrs6.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs6.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs6.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs6.append(MakeHttp1HeaderLine(":path", "/trailers-nobody"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCodeOK, HeadersView(hdrs6), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  // HttpResponse enforces that trailers can only be emitted after a non-empty body;
  // the handler catches that exception and returns 500.
  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "500");
  ASSERT_FALSE(loop.clientData.empty());
  EXPECT_TRUE(loop.clientData[0].data.contains("Trailers must be added after a non empty body is set"));
  EXPECT_TRUE(loop.clientData.back().endStream);
}

TEST(Http2ProtocolHandler, ParsesManyHttpMethodsAndFallsBackToGetForUnknown) {
  Router router;
  vector<http::Method> seen;

  router.setDefault([&seen](const HttpRequest& req) {
    seen.push_back(req.method());
    return HttpResponse(200);
  });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  struct MethodCase {
    uint32_t streamId;
    std::string_view method;
    http::Method expected;
    bool reachesHandler;  // false for methods handled before reaching handler
  };

  // TRACE in HTTP/2 returns 405 since there's no wire format to echo (per RFC 9113).
  // It gets handled by ProcessSpecialMethods and never reaches the default handler.
  // CONNECT also does not reach the handler; it returns 405 since tunneling is not yet implemented.
  const std::array<MethodCase, 9> cases = {
      MethodCase{1, "PUT", http::Method::PUT, true},       MethodCase{3, "DELETE", http::Method::DELETE, true},
      MethodCase{5, "HEAD", http::Method::HEAD, true},     MethodCase{7, "OPTIONS", http::Method::OPTIONS, true},
      MethodCase{9, "PATCH", http::Method::PATCH, true},   MethodCase{11, "CONNECT", http::Method::CONNECT, false},
      MethodCase{13, "TRACE", http::Method::TRACE, false},  // HTTP/2 TRACE -> 405 before handler
      MethodCase{15, "POST", http::Method::POST, true},    MethodCase{17, "BREW", http::Method::GET, true},
  };

  for (const auto& tc : cases) {
    RawChars mhdrs;
    mhdrs.append(MakeHttp1HeaderLine(":method", std::string(tc.method)));
    mhdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
    mhdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
    // CONNECT omits :path per RFC 7540 §8.3, but other methods require it
    if (tc.method != "CONNECT") {
      mhdrs.append(MakeHttp1HeaderLine(":path", "/m"));
    }
    mhdrs.append(MakeHttp1HeaderLine(":unknown", "ignored"));
    const auto ok = loop.client.sendHeaders(tc.streamId, http::StatusCode{}, HeadersView(mhdrs), true);
    ASSERT_EQ(ok, ErrorCode::NoError);
    loop.pumpClientToServer();
    loop.pumpServerToClient();
  }

  // Count expected handler invocations (methods that reach the handler)
  const auto expectedCount = std::ranges::count_if(cases, [](const auto& tc) { return tc.reachesHandler; });
  ASSERT_EQ(seen.size(), static_cast<size_t>(expectedCount));

  // Verify the methods that do reach the handler
  size_t seenIdx = 0;
  for (const auto& tc : cases) {
    if (tc.reachesHandler) {
      EXPECT_EQ(seen[static_cast<uint32_t>(seenIdx)], tc.expected);
      ++seenIdx;
    }
  }
}

TEST(Http2ProtocolHandler, SetsPathParamsFromRouterMatch) {
  Router router;
  router.setPath(http::Method::GET, "/items/{id}/view", [](const HttpRequest& req) {
    const auto& pp = req.pathParams();
    EXPECT_TRUE(pp.contains("id"));
    EXPECT_EQ(pp.at("id"), "42");
    return HttpResponse(200);
  });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs7;
  hdrs7.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs7.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs7.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs7.append(MakeHttp1HeaderLine(":path", "/items/42/view"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCodeOK, HeadersView(hdrs7), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
}

TEST(Http2ProtocolHandler, PerRouteHttp2DisableReturns404) {
  Router router;
  router.setPath(http::Method::GET, "/h1only", [](const HttpRequest&) { return HttpResponse(200); })
      .http2Enable(::aeronet::PathEntryConfig::Http2Enable::Disable);

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs8;
  hdrs8.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs8.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs8.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs8.append(MakeHttp1HeaderLine(":path", "/h1only"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCodeOK, HeadersView(hdrs8), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "404");
}

TEST(Http2ProtocolHandler, UnknownPathReturns404) {
  Router router;
  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs9;
  hdrs9.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs9.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs9.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs9.append(MakeHttp1HeaderLine(":path", "/nope"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCodeOK, HeadersView(hdrs9), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "404");
}

TEST(Http2ProtocolHandler, TransportClosingClearsPendingStreamRequests) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs3;
  hdrs3.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs3.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs3.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs3.append(MakeHttp1HeaderLine(":path", "/body"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCodeOK, HeadersView(hdrs3), false);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.handler.onTransportClosing();

  const std::array<std::byte, 3> body = {std::byte{'a'}, std::byte{'b'}, std::byte{'c'}};
  ASSERT_EQ(loop.client.sendData(1, body, false), ErrorCode::NoError);
  loop.pumpClientToServer();

  EXPECT_FALSE(loop.handler.hasPendingOutput());
}

TEST(Http2ProtocolHandler, StreamResetAndClosedCallbacksEraseStreamState) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs10;
  hdrs10.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs10.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs10.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs10.append(MakeHttp1HeaderLine(":path", "/reset"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCodeOK, HeadersView(hdrs10), false);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();

  loop.client.sendRstStream(1, ErrorCode::Cancel);
  loop.pumpClientToServer();
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
TEST(Http2ProtocolHandler, AsyncHandlerRunsToCompletion) {
  Router router;
  router.setPath(http::Method::GET, "/async",
                 [](HttpRequest&) -> RequestTask<HttpResponse> { co_return HttpResponse("async-ok"); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs11;
  hdrs11.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs11.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs11.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs11.append(MakeHttp1HeaderLine(":path", "/async"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCodeOK, HeadersView(hdrs11), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
  ASSERT_FALSE(loop.clientData.empty());
  EXPECT_EQ(loop.clientData[0].data, "async-ok");
}

TEST(Http2ProtocolHandler, AsyncHandlerInvalidTaskReturns500) {
  Router router;
  router.setPath(http::Method::GET, "/async-invalid", [](HttpRequest&) -> RequestTask<HttpResponse> { return {}; });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs12;
  hdrs12.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs12.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs12.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs12.append(MakeHttp1HeaderLine(":path", "/async-invalid"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCodeOK, HeadersView(hdrs12), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "500");
  ASSERT_FALSE(loop.clientData.empty());
  EXPECT_EQ(loop.clientData[0].data, "Async handler inactive");
}
#endif

TEST(Http2ProtocolHandler, StreamingHandlerReturns501NotImplemented) {
  Router router;
  router.setPath(http::Method::GET, "/stream",
                 ::aeronet::StreamingHandler{[]([[maybe_unused]] const HttpRequest& req,
                                                [[maybe_unused]] ::aeronet::HttpResponseWriter& writer) {}});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs13;
  hdrs13.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs13.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs13.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs13.append(MakeHttp1HeaderLine(":path", "/stream"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs13), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "501");
  ASSERT_FALSE(loop.clientData.empty());
  EXPECT_TRUE(loop.clientData[0].data.contains("not yet supported"));
}

TEST(Http2ProtocolHandler, MethodNotAllowedReturns405) {
  Router router;
  router.setPath(http::Method::GET, "/onlyget", [](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs4;
  hdrs4.append(MakeHttp1HeaderLine(":method", "POST"));
  hdrs4.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs4.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs4.append(MakeHttp1HeaderLine(":path", "/onlyget"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs4), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "405");
}

TEST(Http2ProtocolHandler, HandlerExceptionReturns500WithMessage) {
  Router router;
  router.setPath(http::Method::GET, "/boom",
                 [](const HttpRequest&) -> HttpResponse { throw std::runtime_error("boom"); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs14;
  hdrs14.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs14.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs14.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs14.append(MakeHttp1HeaderLine(":path", "/boom"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCodeOK, HeadersView(hdrs14), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "500");
  ASSERT_FALSE(loop.clientData.empty());
  EXPECT_EQ(loop.clientData[0].data, "boom");
}

TEST(Http2ProtocolHandler, HandlerUnknownExceptionReturns500UnknownError) {
  Router router;
  router.setPath(http::Method::GET, "/boom2", [](const HttpRequest&) -> HttpResponse { throw 42; });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs15;
  hdrs15.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs15.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs15.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs15.append(MakeHttp1HeaderLine(":path", "/boom2"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCodeOK, HeadersView(hdrs15), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "500");
  ASSERT_FALSE(loop.clientData.empty());
  EXPECT_EQ(loop.clientData[0].data, "Unknown error");
}

TEST(Http2ProtocolHandler, MissingPathSendsRstStream) {
  Router router;
  // Default handler should not be called because request is invalid
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  // Send headers without :path pseudo-header
  RawChars hdrs16;
  hdrs16.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs16.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs16.append(MakeHttp1HeaderLine(":authority", "example.com"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCodeOK, HeadersView(hdrs16), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  // Deliver server output to the client so the RST_STREAM is observed by the client
  loop.pumpServerToClient();

  // The handler should send a RST_STREAM (client receives stream reset)
  // Client side registers resets in streamResets vector
  ASSERT_FALSE(loop.streamResets.empty());
  // Expect the reset for stream 1 with ProtocolError
  const auto [sid, code] = loop.streamResets.back();
  EXPECT_EQ(sid, 1U);
  EXPECT_EQ(code, ErrorCode::ProtocolError);
}

// ============== HTTP/2 Special Methods Tests ==============

TEST(Http2ProtocolHandler, OptionsStarReturnsAllowedMethods) {
  Router router;
  // Need a default handler for OPTIONS * to return all methods
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });
  router.setPath(http::Method::GET, "/a", [](const HttpRequest&) { return HttpResponse(200); });
  router.setPath(http::Method::POST, "/b", [](const HttpRequest&) { return HttpResponse(201); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "OPTIONS"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "*"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  const auto& resp = loop.clientHeaders.back();
  EXPECT_EQ(GetHeaderValue(resp, ":status"), "200");
  const auto allow = GetHeaderValue(resp, "allow");
  // With a default handler, all methods should be allowed
  EXPECT_FALSE(allow.empty());
}

TEST(Http2ProtocolHandler, OptionsPathWithoutHandlerReturns405) {
  Router router;
  // Register GET and POST for /users but NOT OPTIONS
  router.setPath(http::Method::GET, "/users", [](const HttpRequest&) { return HttpResponse(200); });
  router.setPath(http::Method::POST, "/users", [](const HttpRequest&) { return HttpResponse(201); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  // OPTIONS /users without a registered handler and no CORS returns 405
  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "OPTIONS"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/users"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  const auto& resp = loop.clientHeaders.back();
  // Without CORS preflight or a registered OPTIONS handler, 405 is expected
  EXPECT_EQ(GetHeaderValue(resp, ":status"), "405");
}

TEST(Http2ProtocolHandler, TraceReturns405InHttp2) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  // TRACE must return 405 in HTTP/2 because there's no wire format to echo
  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "TRACE"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/test"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  const auto& resp = loop.clientHeaders.back();
  EXPECT_EQ(GetHeaderValue(resp, ":status"), "405");
}

TEST(Http2ProtocolHandler, CorsPreflightReturnsAllowOrigin) {
  Router router;

  CorsPolicy cors(CorsPolicy::Active::On);
  cors.allowOrigin("https://allowed.example.com").allowMethods(http::Method::POST);

  router.setPath(http::Method::POST, "/api/data", [](const HttpRequest&) { return HttpResponse(201); })
      .cors(std::move(cors));

  Http2ProtocolLoopback loop(router);
  loop.connect();

  // Send CORS preflight
  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "OPTIONS"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/api/data"));
  hdrs.append(MakeHttp1HeaderLine("origin", "https://allowed.example.com"));
  hdrs.append(MakeHttp1HeaderLine("access-control-request-method", "POST"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  const auto& resp = loop.clientHeaders.back();
  EXPECT_EQ(GetHeaderValue(resp, ":status"), "204");
  EXPECT_EQ(GetHeaderValue(resp, "access-control-allow-origin"), "https://allowed.example.com");
}

TEST(Http2ProtocolHandler, CorsPreflightDeniesUnallowedOrigin) {
  Router router;

  CorsPolicy cors(CorsPolicy::Active::On);
  cors.allowOrigin("https://allowed.example.com");

  router.setPath(http::Method::POST, "/api/data", [](const HttpRequest&) { return HttpResponse(201); })
      .cors(std::move(cors));

  Http2ProtocolLoopback loop(router);
  loop.connect();

  // Send CORS preflight from non-allowed origin
  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "OPTIONS"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/api/data"));
  hdrs.append(MakeHttp1HeaderLine("origin", "https://evil.example.com"));
  hdrs.append(MakeHttp1HeaderLine("access-control-request-method", "POST"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  const auto& resp = loop.clientHeaders.back();
  EXPECT_EQ(GetHeaderValue(resp, ":status"), "403");
}

TEST(Http2ProtocolHandler, RequestMiddlewareExecutes) {
  Router router;
  bool middlewareCalled = false;
  bool handlerCalled = false;

  router.addRequestMiddleware([&middlewareCalled](HttpRequest&) {
    middlewareCalled = true;
    return MiddlewareResult::Continue();
  });

  router.setPath(http::Method::GET, "/test", [&handlerCalled](const HttpRequest&) {
    handlerCalled = true;
    return HttpResponse(200);
  });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/test"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  EXPECT_TRUE(middlewareCalled);
  EXPECT_TRUE(handlerCalled);
  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "200");
}

TEST(Http2ProtocolHandler, RequestMiddlewareCanShortCircuit) {
  Router router;
  bool handlerCalled = false;

  router.addRequestMiddleware([](HttpRequest&) {
    // Short-circuit with 403
    return MiddlewareResult::ShortCircuit(HttpResponse(403));
  });

  router.setPath(http::Method::GET, "/test", [&handlerCalled](const HttpRequest&) {
    handlerCalled = true;
    return HttpResponse(200);
  });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/test"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  EXPECT_FALSE(handlerCalled);  // Handler should NOT be called
  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "403");
}

TEST(Http2ProtocolHandler, ResponseMiddlewareExecutes) {
  Router router;

  router.addResponseMiddleware(
      [](const HttpRequest&, HttpResponse& resp) { resp.header("X-Middleware-Added", "test-value"); });

  router.setPath(http::Method::GET, "/test", [](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/test"));
  const auto ok = loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  const auto& resp = loop.clientHeaders.back();
  EXPECT_EQ(GetHeaderValue(resp, ":status"), "200");
  EXPECT_EQ(GetHeaderValue(resp, "x-middleware-added"), "test-value");
}

TEST(Http2ProtocolHandler, RejectsWhenClientForbidsIdentityWithoutAcceptableEncoding) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2Config serverCfg;
  Http2Config clientCfg;
  HttpServerConfig serverConfig;

  // Configure server with NO supported encodings
  serverConfig.compression.preferredFormats.clear();

  internal::ResponseCompressionState compressionState(serverConfig.compression);
  internal::RequestDecompressionState decompressionState;

  Http2ProtocolHandler handler(serverCfg, router, serverConfig, compressionState, decompressionState, telemetry,
                               tmpBuffer);
  Http2Connection client(clientCfg, false);
  ::aeronet::ConnectionState state;

  vector<HeaderEvent> clientHeaders;
  client.setOnHeadersDecoded([&clientHeaders](uint32_t streamId, const HeadersViewMap& headers, bool endStream) {
    HeaderEvent ev;
    ev.streamId = streamId;
    ev.endStream = endStream;
    for (const auto& [name, value] : headers) {
      ev.headers.emplace_back(name, value);
    }
    clientHeaders.push_back(std::move(ev));
  });

  // Establish connection: send client preface
  client.sendClientPreface();
  {
    auto out = client.getPendingOutput();
    std::span<const std::byte> bytes = out;
    std::size_t safetyIters = 0;
    while (!bytes.empty()) {
      ++safetyIters;
      if (safetyIters > 64U) {
        ADD_FAILURE() << "feedHandler got stuck";
        return;
      }
      auto res = handler.processInput(bytes, state);
      if (res.bytesConsumed == 0) {
        ADD_FAILURE() << "No progress feeding handler";
        return;
      }
      bytes = bytes.subspan(res.bytesConsumed);
    }
    client.onOutputWritten(client.getPendingOutput().size());
  }

  // Pump server response
  while (handler.hasPendingOutput()) {
    auto out = handler.getPendingOutput();
    std::span<const std::byte> bytes = out;
    std::size_t safetyIters = 0;
    while (!bytes.empty()) {
      ++safetyIters;
      if (safetyIters > 64U) {
        ADD_FAILURE() << "feedConn got stuck";
        return;
      }
      const auto prevState = client.state();
      auto res = client.processInput(bytes);
      if (res.action == Http2Connection::ProcessResult::Action::Error ||
          res.action == Http2Connection::ProcessResult::Action::Closed ||
          res.action == Http2Connection::ProcessResult::Action::GoAway) {
        if (res.bytesConsumed > 0) {
          bytes = bytes.subspan(res.bytesConsumed);
        }
        break;
      }
      if (res.bytesConsumed > 0) {
        bytes = bytes.subspan(res.bytesConsumed);
        continue;
      }
      if (client.state() != prevState) {
        continue;
      }
      break;
    }
    handler.onOutputWritten(out.size());
  }

  // Send request with Accept-Encoding that explicitly forbids identity with q=0
  // and requests only unsupported encodings. This should trigger rejection.
  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/test"));
  // Accept only unsupported "hypothetical-encoding" and forbid identity explicitly
  hdrs.append(MakeHttp1HeaderLine("accept-encoding", "hypothetical-encoding, identity;q=0"));
  const auto ok = client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true);
  ASSERT_EQ(ok, ErrorCode::NoError);

  // Pump client request to server
  {
    auto out = client.getPendingOutput();
    std::span<const std::byte> bytes = out;
    std::size_t safetyIters = 0;
    while (!bytes.empty()) {
      ++safetyIters;
      if (safetyIters > 64U) {
        ADD_FAILURE() << "feedHandler got stuck";
        return;
      }
      auto res = handler.processInput(bytes, state);
      if (res.bytesConsumed == 0) {
        ADD_FAILURE() << "No progress feeding handler";
        return;
      }
      bytes = bytes.subspan(res.bytesConsumed);
    }
    client.onOutputWritten(client.getPendingOutput().size());
  }

  // Pump server response back to client
  while (handler.hasPendingOutput()) {
    auto out = handler.getPendingOutput();
    std::span<const std::byte> bytes = out;
    std::size_t safetyIters = 0;
    while (!bytes.empty()) {
      ++safetyIters;
      if (safetyIters > 64U) {
        ADD_FAILURE() << "feedConn got stuck";
        return;
      }
      const auto prevState = client.state();
      auto res = client.processInput(bytes);
      if (res.action == Http2Connection::ProcessResult::Action::Error ||
          res.action == Http2Connection::ProcessResult::Action::Closed ||
          res.action == Http2Connection::ProcessResult::Action::GoAway) {
        if (res.bytesConsumed > 0) {
          bytes = bytes.subspan(res.bytesConsumed);
        }
        break;
      }
      if (res.bytesConsumed > 0) {
        bytes = bytes.subspan(res.bytesConsumed);
        continue;
      }
      if (client.state() != prevState) {
        continue;
      }
      break;
    }
    handler.onOutputWritten(out.size());
  }

  // Verify that server responded with 406 Not Acceptable
  ASSERT_FALSE(clientHeaders.empty());
  const auto& resp = clientHeaders.back();
  EXPECT_EQ(GetHeaderValue(resp, ":status"), "406");
}

}  // namespace aeronet::http2
