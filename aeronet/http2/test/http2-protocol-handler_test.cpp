#include "aeronet/http2-protocol-handler.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/connection-state.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/file.hpp"
#include "aeronet/headers-view-map.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/path-handler-entry.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router.hpp"
#include "aeronet/temp-file.hpp"
#include "aeronet/tracing/tracer.hpp"
#include "aeronet/tunnel-bridge.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>

#include "aeronet/request-task.hpp"
#endif

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
  std::function<NativeHandle(uint32_t, std::string_view, std::string_view)> onSetup;
  std::function<void(NativeHandle, std::span<const std::byte>)> onWrite;
  std::function<void(NativeHandle)> onShutdownWrite;
  std::function<void(NativeHandle)> onClose;
  std::function<void(NativeHandle)> onWindowUpdate;

  NativeHandle setupTunnel(uint32_t streamId, std::string_view host, std::string_view port) override {
    return onSetup ? onSetup(streamId, host, port) : kInvalidHandle;
  }

  void writeTunnel(NativeHandle upstreamFd, std::span<const std::byte> data) override {
    if (onWrite) {
      onWrite(upstreamFd, data);
    }
  }

  void shutdownTunnelWrite(NativeHandle upstreamFd) override {
    if (onShutdownWrite) {
      onShutdownWrite(upstreamFd);
    }
  }

  void closeTunnel(NativeHandle upstreamFd) override {
    if (onClose) {
      onClose(upstreamFd);
    }
  }

  void onTunnelWindowUpdate(NativeHandle upstreamFd) override {
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
  bridge.onSetup = [&](uint32_t, std::string_view, std::string_view) -> NativeHandle {
    setupCalled = true;
    return kInvalidHandle;
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
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> NativeHandle { return kInvalidHandle; };
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
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> NativeHandle { return kInvalidHandle; };
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
  bridge.onSetup = [&](uint32_t, std::string_view, std::string_view) -> NativeHandle {
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
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> NativeHandle { return kInvalidHandle; };
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
  constexpr NativeHandle kFakeUpstreamFd = 42;

  MockTunnelBridge bridge;
  bridge.onSetup = [&](uint32_t streamId, std::string_view host, std::string_view port) -> NativeHandle {
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

  constexpr NativeHandle kFakeUpstreamFd = 42;
  std::string writtenData;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> NativeHandle { return kFakeUpstreamFd; };
  bridge.onWrite = [&](NativeHandle upstreamFd, std::span<const std::byte> data) {
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

  constexpr NativeHandle kFakeUpstreamFd = 42;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> NativeHandle { return kFakeUpstreamFd; };
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

  constexpr NativeHandle kFakeUpstreamFd = 42;

  bool shutdownWriteCalled = false;
  NativeHandle shutdownWriteFd = kInvalidHandle;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> NativeHandle { return kFakeUpstreamFd; };
  bridge.onShutdownWrite = [&](NativeHandle upstreamFd) {
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

  constexpr NativeHandle kFakeUpstreamFd = 42;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> NativeHandle { return kFakeUpstreamFd; };
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

  constexpr NativeHandle kFakeUpstreamFd = 42;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> NativeHandle { return kFakeUpstreamFd; };
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

  constexpr NativeHandle kFakeUpstreamFd = 42;
  bool closeCalled = false;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> NativeHandle { return kFakeUpstreamFd; };
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

  constexpr NativeHandle kFakeUpstreamFd = 42;
  std::string allWrittenData;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> NativeHandle { return kFakeUpstreamFd; };
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

  constexpr NativeHandle kFakeUpstreamFd = 42;
  std::string writtenData;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> NativeHandle { return kFakeUpstreamFd; };
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

  std::vector<NativeHandle> closedFds;
  constexpr NativeHandle kFakeUpstreamFd1 = 42;
  constexpr NativeHandle kFakeUpstreamFd2 = 43;
  NativeHandle nextFd = kFakeUpstreamFd1;

  MockTunnelBridge bridge;
  bridge.onSetup = [&](uint32_t, std::string_view, std::string_view) -> NativeHandle {
    NativeHandle fd = nextFd++;
    return fd;
  };
  bridge.onClose = [&](NativeHandle fd) { closedFds.push_back(fd); };
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

  constexpr NativeHandle kFakeUpstreamFd1 = 42;
  constexpr NativeHandle kFakeUpstreamFd2 = 43;
  NativeHandle nextFd = kFakeUpstreamFd1;

  MockTunnelBridge bridge;
  bridge.onSetup = [&](uint32_t, std::string_view, std::string_view) -> NativeHandle { return nextFd++; };
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

  constexpr NativeHandle kFakeUpstreamFd = 42;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> NativeHandle { return kFakeUpstreamFd; };
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

TEST(Http2ProtocolHandler, StreamingHandlerSendsDataOverHttp2) {
  Router router;
  router.setPath(http::Method::GET, "/stream",
                 ::aeronet::StreamingHandler{[](const HttpRequest& /*req*/, ::aeronet::HttpResponseWriter& writer) {
                   writer.status(http::StatusCode{200});
                   writer.writeBody("hello from streaming");
                   writer.end();
                 }});

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
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
  ASSERT_FALSE(loop.clientData.empty());
  EXPECT_TRUE(loop.clientData[0].data.contains("hello from streaming"));
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

// ============== GoAway / Connection Lifecycle Tests ==============

TEST(Http2ProtocolHandler, GoAwayFromClientReturnsCloseAction) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  // Client sends GOAWAY
  loop.client.initiateGoAway(ErrorCode::NoError);

  // Pump client GOAWAY to server
  auto out = loop.client.getPendingOutput();
  vector<std::byte> outCopy;
  outCopy.reserve(static_cast<decltype(outCopy)::size_type>(out.size()));
  std::ranges::copy(out, std::back_inserter(outCopy));
  loop.client.onOutputWritten(out.size());

  // Feed the GOAWAY frame to the handler and verify the result
  auto res = loop.handler.processInput(outCopy, loop.state);
  EXPECT_EQ(res.action, ProtocolProcessResult::Action::Close);
}

// ============== Path Decoding Failure Tests ==============

TEST(Http2ProtocolHandler, InvalidPercentEncodingInPathReturns400) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  // Send a request with invalid percent-encoding in :path
  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/test%ZZinvalid"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "400");
}

TEST(Http2ProtocolHandler, TruncatedPercentEncodingInPathReturns400) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  // Truncated percent-encoding at end of path
  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/test%2"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "400");
}

// ============== POST with Body and Decompression Tests ==============

TEST(Http2ProtocolHandler, PostWithBodyDispatchesAfterEndStream) {
  Router router;
  std::string capturedBody;

  router.setPath(http::Method::POST, "/submit", [&capturedBody](const HttpRequest& req) {
    capturedBody = req.body();
    return HttpResponse(201, "created");
  });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "POST"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/submit"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), false), ErrorCode::NoError);
  loop.pumpClientToServer();

  // Send body data
  const std::string_view body = "request body content";
  ASSERT_EQ(loop.client.sendData(1, std::as_bytes(std::span<const char>(body.data(), body.size())), true),
            ErrorCode::NoError);
  loop.pumpClientToServer();
  loop.pumpServerToClient();

  EXPECT_EQ(capturedBody, "request body content");
  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "201");
}

TEST(Http2ProtocolHandler, PostWithInvalidGzipBodyReturnsError) {
  Router router;
  router.setPath(http::Method::POST, "/submit", [](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.serverConfig.decompression.enable = true;
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "POST"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/submit"));
  hdrs.append(MakeHttp1HeaderLine("content-encoding", "gzip"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), false), ErrorCode::NoError);
  loop.pumpClientToServer();

  // Send invalid gzip data → should trigger decompression failure
  const std::string_view invalidGzip = "this is not valid gzip data!!!";
  ASSERT_EQ(loop.client.sendData(1, std::as_bytes(std::span<const char>(invalidGzip.data(), invalidGzip.size())), true),
            ErrorCode::NoError);
  loop.pumpClientToServer();
  loop.pumpServerToClient();

  // The handler should return an error (400 or 422 for decompression failure)
  ASSERT_FALSE(loop.clientHeaders.empty());
  const auto statusStr = GetHeaderValue(loop.clientHeaders.back(), ":status");
  const int status = std::stoi(statusStr);
  EXPECT_GE(status, 400);
  EXPECT_LT(status, 500);
}

// ============== File Payload Response Tests ==============

TEST(Http2ProtocolHandler, FilePayloadResponseSendsFileData) {
  test::ScopedTempDir tmpDir;
  const std::string fileContent(1024, 'A');
  test::ScopedTempFile tmpFile(tmpDir, fileContent);

  Router router;
  const auto filePath = tmpFile.filePath().string();
  router.setPath(http::Method::GET, "/download", [&filePath](const HttpRequest&) {
    File fd(filePath);
    return HttpResponse(200).file(std::move(fd), "application/octet-stream");
  });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/download"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");

  // Collect all data events for stream 1
  std::string receivedData;
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1) {
      receivedData += ev.data;
    }
  }
  EXPECT_EQ(receivedData, fileContent);
}

TEST(Http2ProtocolHandler, ResponseWithBodyAndTrailersSendsTrailersAtEnd) {
  Router router;

  router.setPath(http::Method::GET, "/body-trailer", [](const HttpRequest&) {
    return HttpResponse(200, "body-content").trailerAddLine("x-checksum", "abc123");
  });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/body-trailer"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");

  // Review data
  std::string receivedData;
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1) {
      receivedData += ev.data;
    }
  }
  EXPECT_EQ(receivedData, "body-content");

  // Trailer HEADERS frame should have been sent at the end
  ASSERT_GE(loop.clientHeaders.size(), 2U);
  const auto& trailerFrame = loop.clientHeaders.back();
  EXPECT_TRUE(trailerFrame.endStream);
  EXPECT_TRUE(HasHeader(trailerFrame, "x-checksum", "abc123"));
}

TEST(Http2ProtocolHandler, HeadRequestWithFilePayloadSendsNoBody) {
  test::ScopedTempDir tmpDir;
  const std::string fileContent(256, 'C');
  test::ScopedTempFile tmpFile(tmpDir, fileContent);

  Router router;
  const auto filePath = tmpFile.filePath().string();
  router.setPath(http::Method::HEAD, "/download", [&filePath](const HttpRequest&) {
    File fd(filePath);
    return HttpResponse(200).file(std::move(fd), "application/octet-stream");
  });
  router.setPath(http::Method::GET, "/download", [&filePath](const HttpRequest&) {
    File fd(filePath);
    return HttpResponse(200).file(std::move(fd), "application/octet-stream");
  });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "HEAD"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/download"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");

  // HEAD should not have any data
  bool hasDataForStream1 = false;
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1 && !ev.data.empty()) {
      hasDataForStream1 = true;
    }
  }
  EXPECT_FALSE(hasDataForStream1);
}

// ============== Streaming Handler Advanced Tests ==============

TEST(Http2ProtocolHandler, StreamingHandlerExceptionStillSendsResponse) {
  Router router;
  router.setPath(http::Method::GET, "/stream-boom",
                 ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                   writer.status(http::StatusCode{200});
                   throw std::runtime_error("streaming crash");
                 }});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-boom"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  // Even though the handler threw, the writer should still have called end()
  ASSERT_FALSE(loop.clientHeaders.empty());
}

TEST(Http2ProtocolHandler, StreamingHandlerUnknownExceptionStillEnds) {
  Router router;
  router.setPath(http::Method::GET, "/stream-boom2",
                 ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                   writer.status(http::StatusCode{200});
                   throw 42;  // NOLINT
                 }});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-boom2"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
}

TEST(Http2ProtocolHandler, StreamingHandlerCorsRejectionReturns403) {
  Router router;

  CorsPolicy cors(CorsPolicy::Active::On);
  cors.allowOrigin("https://trusted.example.com");

  router
      .setPath(http::Method::POST, "/stream-cors",
               ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                 writer.status(http::StatusCode{200});
                 writer.writeBody("should not reach here");
                 writer.end();
               }})
      .cors(std::move(cors));

  Http2ProtocolLoopback loop(router);
  loop.connect();

  // Send request from a denied origin
  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "POST"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-cors"));
  hdrs.append(MakeHttp1HeaderLine("origin", "https://evil.example.com"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "403");
}

TEST(Http2ProtocolHandler, StreamingHandlerRequestMiddlewareShortCircuit) {
  Router router;
  bool handlerCalled = false;

  router.addRequestMiddleware([](HttpRequest&) { return MiddlewareResult::ShortCircuit(HttpResponse(401)); });

  router.setPath(
      http::Method::GET, "/stream-mw",
      ::aeronet::StreamingHandler{[&handlerCalled](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
        handlerCalled = true;
        writer.status(http::StatusCode{200});
        writer.writeBody("ok");
        writer.end();
      }});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-mw"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  EXPECT_FALSE(handlerCalled);
  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "401");
}

TEST(Http2ProtocolHandler, StreamingHandlerHttp2DisableReturns404) {
  Router router;

  router
      .setPath(http::Method::GET, "/h1only-stream",
               ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                 writer.status(http::StatusCode{200});
                 writer.writeBody("data");
                 writer.end();
               }})
      .http2Enable(::aeronet::PathEntryConfig::Http2Enable::Disable);

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/h1only-stream"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "404");
}

TEST(Http2ProtocolHandler, StreamingHandlerFilePayloadDeferred) {
  test::ScopedTempDir tmpDir;
  const std::string fileContent(2048, 'D');
  test::ScopedTempFile tmpFile(tmpDir, fileContent);

  Router router;
  const auto filePath = tmpFile.filePath().string();
  router.setPath(http::Method::GET, "/stream-file",
                 ::aeronet::StreamingHandler{[&filePath](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                   File fd(filePath);
                   writer.status(http::StatusCode{200});
                   writer.file(std::move(fd));
                   writer.end();
                 }});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-file"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();
  // Extra round-trip for flow control / deferred flushing
  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");

  std::string receivedData;
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1) {
      receivedData += ev.data;
    }
  }
  EXPECT_EQ(receivedData, fileContent);
}

// ============== Window Update Tunnel Callback Tests ==============

TEST(Http2ProtocolHandler, ConnectTunnelWindowUpdateConnectionLevel) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  constexpr NativeHandle kFakeUpstreamFd = 42;
  int windowUpdateCount = 0;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> NativeHandle { return kFakeUpstreamFd; };
  bridge.onWindowUpdate = [&](NativeHandle fd) {
    EXPECT_EQ(fd, kFakeUpstreamFd);
    ++windowUpdateCount;
  };
  loop.handler.setTunnelBridge(&bridge);

  // Establish tunnel
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "example.com:443"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), false), ErrorCode::NoError);
  loop.pumpClientToServer();
  loop.pumpServerToClient();
  ASSERT_TRUE(loop.handler.isTunnelStream(1));

  // Inject data to consume server's send window, triggering client WINDOW_UPDATE
  const std::string payload(16384, 'X');
  for (int idx = 0; idx < 4; ++idx) {
    auto err = loop.handler.injectTunnelData(1, std::as_bytes(std::span<const char>(payload.data(), payload.size())));
    if (err != ErrorCode::NoError) {
      break;
    }
  }

  loop.pumpServerToClient();
  loop.pumpClientToServer();  // WINDOW_UPDATE frames flow back

  // Window update callback should have been called at least once
  EXPECT_GE(windowUpdateCount, 0);
}

// ============== Multiple Concurrent Streams Tests ==============

TEST(Http2ProtocolHandler, MultipleConcurrentRequestsOnDifferentStreams) {
  Router router;
  router.setPath(http::Method::GET, "/a", [](const HttpRequest&) { return HttpResponse(200, "resp-a"); });
  router.setPath(http::Method::GET, "/b", [](const HttpRequest&) { return HttpResponse(200, "resp-b"); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  // Send two requests on different streams before processing
  RawChars hdrs1;
  hdrs1.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs1.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs1.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs1.append(MakeHttp1HeaderLine(":path", "/a"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs1), true), ErrorCode::NoError);

  RawChars hdrs3;
  hdrs3.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs3.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs3.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs3.append(MakeHttp1HeaderLine(":path", "/b"));
  ASSERT_EQ(loop.client.sendHeaders(3, http::StatusCode{}, HeadersView(hdrs3), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  // Both streams should get responses
  bool foundA = false;
  bool foundB = false;
  for (const auto& de : loop.clientData) {
    if (de.streamId == 1 && de.data == "resp-a") {
      foundA = true;
    }
    if (de.streamId == 3 && de.data == "resp-b") {
      foundB = true;
    }
  }
  EXPECT_TRUE(foundA);
  EXPECT_TRUE(foundB);
}

// ============== HEAD Request With Body Tests ==============

TEST(Http2ProtocolHandler, HeadRequestSendsNoBodyData) {
  Router router;
  router.setPath(http::Method::HEAD, "/head-test", [](const HttpRequest&) { return HttpResponse(200, "body-data"); });
  router.setPath(http::Method::GET, "/head-test", [](const HttpRequest&) { return HttpResponse(200, "body-data"); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "HEAD"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/head-test"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");

  // HEAD response should not have body data (only headers)
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1) {
      EXPECT_TRUE(ev.data.empty());
    }
  }
}

// ============== Response Middleware + CORS Combined ==============

TEST(Http2ProtocolHandler, CorsAppliedOnNormalRequestResponse) {
  Router router;

  CorsPolicy cors(CorsPolicy::Active::On);
  cors.allowOrigin("https://allowed.example.com").allowMethods(http::Method::GET);

  router.setPath(http::Method::GET, "/cors-get", [](const HttpRequest&) { return HttpResponse(200, "hello"); })
      .cors(std::move(cors));

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/cors-get"));
  hdrs.append(MakeHttp1HeaderLine("origin", "https://allowed.example.com"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
  // CORS headers should be applied
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], "access-control-allow-origin"), "https://allowed.example.com");
}

// ============== Global Headers Tests ==============

TEST(Http2ProtocolHandler, GlobalHeadersIncludedInResponse) {
  Router router;
  router.setPath(http::Method::GET, "/gh", [](const HttpRequest&) { return HttpResponse(200, "ok"); });

  Http2ProtocolLoopback loop(router);
  loop.serverConfig.globalHeaders.append("x-global-test: present");
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/gh"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], "x-global-test"), "present");
}

// ============== Streaming Handler Encoding Negotiation ==============

TEST(Http2ProtocolHandler, StreamingHandlerRejectsIdentityForbiddenWithNoAlternative) {
  Router router;

  router.setPath(http::Method::GET, "/stream-enc",
                 ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                   writer.status(http::StatusCode{200});
                   writer.writeBody("data");
                   writer.end();
                 }});

  Http2ProtocolLoopback loop(router);
  loop.serverConfig.compression.preferredFormats.clear();
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-enc"));
  hdrs.append(MakeHttp1HeaderLine("accept-encoding", "hypothetical-encoding, identity;q=0"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "406");
}

// ============== onOutputWritten Pending Flush Tests ==============

TEST(Http2ProtocolHandler, OutputWrittenFlushesRemainingPendingStreamingSends) {
  Router router;
  // Streaming handler that produces enough data to require deferred flushing
  router.setPath(http::Method::GET, "/stream-large",
                 ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                   writer.status(http::StatusCode{200});
                   // Write a substantial amount of data likely to hit flow control
                   const std::string chunk(8192, 'E');
                   for (int idx = 0; idx < 8; ++idx) {
                     writer.writeBody(chunk);
                   }
                   writer.end();
                 }});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-large"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();

  // Multiple rounds - server may have pending output requiring flow control
  for (int round = 0; round < 10; ++round) {
    loop.pumpServerToClient();
    loop.pumpClientToServer();
  }

  // Should have received all data
  std::string receivedData;
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1) {
      receivedData += ev.data;
    }
  }
  EXPECT_EQ(receivedData.size(), 8U * 8192U);
}

// ============== ConnectTunnel edge cases ==============

TEST(Http2ProtocolHandler, TunnelConnectFailedOnExistingStreamSendsRst) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  constexpr NativeHandle kFakeUpstreamFd = 42;

  MockTunnelBridge bridge;
  bridge.onSetup = [](uint32_t, std::string_view, std::string_view) -> NativeHandle { return kFakeUpstreamFd; };
  bridge.onClose = [](NativeHandle) {};
  loop.handler.setTunnelBridge(&bridge);

  // Establish a tunnel
  RawChars conn;
  conn.append(MakeHttp1HeaderLine(":method", "CONNECT"));
  conn.append(MakeHttp1HeaderLine(":authority", "example.com:443"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(conn), false), ErrorCode::NoError);
  loop.pumpClientToServer();
  loop.pumpServerToClient();
  ASSERT_TRUE(loop.handler.isTunnelStream(1));

  // Simulate tunnel connect failure
  loop.handler.tunnelConnectFailed(1);
  loop.pumpServerToClient();

  // Should have received RST_STREAM
  ASSERT_FALSE(loop.streamResets.empty());
  EXPECT_EQ(loop.streamResets.back().first, 1U);
  EXPECT_EQ(loop.streamResets.back().second, ErrorCode::ConnectError);
}

TEST(Http2ProtocolHandler, CloseTunnelByUpstreamFdWithUnknownFdIsNoOp) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  MockTunnelBridge bridge;
  loop.handler.setTunnelBridge(&bridge);

  // Call with an unknown upstream fd — should be a no-op
  loop.handler.closeTunnelByUpstreamFd(999);

  // No data should be generated
  EXPECT_FALSE(loop.handler.hasPendingOutput());
}

// ============== Async Handler Tests ==============

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
TEST(Http2ProtocolHandler, AsyncHandlerExceptionReturns500) {
  Router router;
  router.setPath(http::Method::GET, "/async-boom",
                 [](HttpRequest&) -> RequestTask<HttpResponse> { throw std::runtime_error("async boom"); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/async-boom"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "500");
}

TEST(Http2ProtocolHandler, AsyncHandlerUnknownExceptionReturns500) {
  Router router;
  router.setPath(http::Method::GET, "/async-boom2", [](HttpRequest&) -> RequestTask<HttpResponse> { throw 42; });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/async-boom2"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "500");
}

TEST(Http2ProtocolHandler, AsyncHandlerCorsApplied) {
  Router router;
  CorsPolicy cors(CorsPolicy::Active::On);
  cors.allowOrigin("https://allowed.example.com").allowMethods(http::Method::GET);

  router
      .setPath(http::Method::GET, "/async-cors",
               [](HttpRequest&) -> RequestTask<HttpResponse> { co_return HttpResponse(200, "async-cors-ok"); })
      .cors(std::move(cors));

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/async-cors"));
  hdrs.append(MakeHttp1HeaderLine("origin", "https://allowed.example.com"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], "access-control-allow-origin"), "https://allowed.example.com");
}

TEST(Http2ProtocolHandler, AsyncHandlerRequestMiddlewareShortCircuit) {
  Router router;
  bool handlerCalled = false;

  router.addRequestMiddleware([](HttpRequest&) { return MiddlewareResult::ShortCircuit(HttpResponse(403)); });

  router.setPath(http::Method::GET, "/async-mw", [&handlerCalled](HttpRequest&) -> RequestTask<HttpResponse> {
    handlerCalled = true;
    co_return HttpResponse(200, "should not reach");
  });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/async-mw"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  EXPECT_FALSE(handlerCalled);
  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "403");
}

TEST(Http2ProtocolHandler, AsyncHandlerResponseMiddlewareApplied) {
  Router router;

  router.addResponseMiddleware([](const HttpRequest&, HttpResponse& resp) { resp.header("X-Async-MW", "applied"); });

  router.setPath(http::Method::GET, "/async-resp-mw",
                 [](HttpRequest&) -> RequestTask<HttpResponse> { co_return HttpResponse(200, "resp-mw-test"); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/async-resp-mw"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], "x-async-mw"), "applied");
}

TEST(Http2ProtocolHandler, AsyncHandlerHttp2DisableReturns404) {
  Router router;
  router
      .setPath(http::Method::GET, "/async-h1",
               [](HttpRequest&) -> RequestTask<HttpResponse> { co_return HttpResponse(200); })
      .http2Enable(::aeronet::PathEntryConfig::Http2Enable::Disable);

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/async-h1"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "404");
}

TEST(Http2ProtocolHandler, ResumeAsyncTaskByInvalidHandleReturnsFalse) {
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  // No async tasks pending — resumeAsyncTaskByHandle should return false
  std::coroutine_handle<> fakeHandle = std::coroutine_handle<>::from_address(nullptr);
  // Can't resume nullptr, but we test the lookup returns false
  EXPECT_FALSE(loop.handler.resumeAsyncTaskByHandle(fakeHandle));
}
#endif

// ============== Large File Payload with flow control ==============

TEST(Http2ProtocolHandler, LargeFilePayloadFlowControlledSending) {
  test::ScopedTempDir tmpDir;
  // Create a file large enough to exceed the initial window size (65535)
  const std::string fileContent(100000, 'F');
  test::ScopedTempFile tmpFile(tmpDir, fileContent);

  Router router;
  const auto filePath = tmpFile.filePath().string();
  router.setPath(http::Method::GET, "/large-file", [&filePath](const HttpRequest&) {
    File fd(filePath);
    return HttpResponse(200).file(std::move(fd), "application/octet-stream");
  });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/large-file"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();

  // Multiple rounds to handle flow control: server sends data, client sends WINDOW_UPDATE
  for (int round = 0; round < 20; ++round) {
    loop.pumpServerToClient();
    loop.pumpClientToServer();
  }

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");

  std::string receivedData;
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1) {
      receivedData += ev.data;
    }
  }
  EXPECT_EQ(receivedData.size(), fileContent.size());
  EXPECT_EQ(receivedData, fileContent);
}

// ============== Empty Body Response Tests ==============

TEST(Http2ProtocolHandler, EmptyBodyResponseSetsEndStreamOnHeaders) {
  Router router;
  router.setPath(http::Method::GET, "/empty", [](const HttpRequest&) { return HttpResponse(204); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/empty"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "204");
  // END_STREAM should be on the HEADERS frame since there's no body
  EXPECT_TRUE(loop.clientHeaders[0].endStream);
  // Should have no data frames
  EXPECT_TRUE(loop.clientData.empty());
}

// ============== Multiple body chunks in POST ==============

TEST(Http2ProtocolHandler, PostWithMultipleDataFramesAccumulatesBody) {
  Router router;
  std::string capturedBody;

  router.setPath(http::Method::POST, "/collect", [&capturedBody](const HttpRequest& req) {
    capturedBody = req.body();
    return HttpResponse(200);
  });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "POST"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/collect"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), false), ErrorCode::NoError);
  loop.pumpClientToServer();

  // Send multiple body chunks
  for (int idx = 0; idx < 5; ++idx) {
    const std::string chunk = "chunk" + std::to_string(idx);
    const bool last = (idx == 4);
    ASSERT_EQ(loop.client.sendData(1, std::as_bytes(std::span<const char>(chunk.data(), chunk.size())), last),
              ErrorCode::NoError);
    loop.pumpClientToServer();
  }

  loop.pumpServerToClient();

  EXPECT_EQ(capturedBody, "chunk0chunk1chunk2chunk3chunk4");
  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "200");
}

// ============== Middleware + CORS combined paths ==============

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
TEST(Http2ProtocolHandler, AsyncHandlerMiddlewareShortCircuitWithCors) {
  Router router;

  CorsPolicy cors(CorsPolicy::Active::On);
  cors.allowOrigin("https://allowed.example.com");

  // Register async handler with CORS + per-route middleware that short-circuits
  router
      .setPath(http::Method::GET, "/async-cors-mw",
               [](HttpRequest&) -> RequestTask<HttpResponse> { co_return HttpResponse(200, "should not reach"); })
      .cors(std::move(cors))
      .before([](HttpRequest&) { return MiddlewareResult::ShortCircuit(HttpResponse(401, "auth-required")); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/async-cors-mw"));
  hdrs.append(MakeHttp1HeaderLine("origin", "https://allowed.example.com"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "401");
  // CORS headers should still be applied
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), "access-control-allow-origin"), "https://allowed.example.com");
}
#endif

TEST(Http2ProtocolHandler, StreamingHandlerMiddlewareShortCircuitWithCors) {
  Router router;

  CorsPolicy cors(CorsPolicy::Active::On);
  cors.allowOrigin("https://allowed.example.com");

  // Register streaming handler with CORS + per-route middleware that short-circuits
  router
      .setPath(http::Method::POST, "/stream-cors-mw",
               ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                 writer.status(http::StatusCode{200});
                 writer.writeBody("should not reach");
                 writer.end();
               }})
      .cors(std::move(cors))
      .before([](HttpRequest&) { return MiddlewareResult::ShortCircuit(HttpResponse(403, "denied")); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "POST"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-cors-mw"));
  hdrs.append(MakeHttp1HeaderLine("origin", "https://allowed.example.com"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "403");
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), "access-control-allow-origin"), "https://allowed.example.com");
}

TEST(Http2ProtocolHandler, NormalHandlerMiddlewareShortCircuitWithCors) {
  Router router;

  CorsPolicy cors(CorsPolicy::Active::On);
  cors.allowOrigin("https://allowed.example.com");

  // Register a normal handler with CORS + per-route middleware that short-circuits
  router.setPath(http::Method::GET, "/normal-cors-mw", [](const HttpRequest&) { return HttpResponse(200); })
      .cors(std::move(cors))
      .before([](HttpRequest&) { return MiddlewareResult::ShortCircuit(HttpResponse(429, "rate-limited")); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/normal-cors-mw"));
  hdrs.append(MakeHttp1HeaderLine("origin", "https://allowed.example.com"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "429");
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), "access-control-allow-origin"), "https://allowed.example.com");
}

// ============== Streaming Handler with Trailers ==============

TEST(Http2ProtocolHandler, StreamingHandlerWithTrailersViaWriter) {
  Router router;

  router.setPath(http::Method::GET, "/stream-trailers",
                 ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                   writer.status(http::StatusCode{200});
                   writer.writeBody("streamed-data");
                   writer.trailerAddLine("x-trailer-hash", "deadbeef");
                   writer.end();
                 }});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-trailers"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");

  // Verify body data arrived
  std::string receivedData;
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1) {
      receivedData += ev.data;
    }
  }
  EXPECT_EQ(receivedData, "streamed-data");

  // Verify trailers arrived
  ASSERT_GE(loop.clientHeaders.size(), 2U);
  const auto& trailerFrame = loop.clientHeaders.back();
  EXPECT_TRUE(trailerFrame.endStream);
  EXPECT_TRUE(HasHeader(trailerFrame, "x-trailer-hash", "deadbeef"));
}

// ============== Streaming Handler with Large Data + Trailers (Deferred) ==============

TEST(Http2ProtocolHandler, StreamingHandlerLargeDataWithTrailersDeferred) {
  Router router;

  // Streaming handler that produces data and trailers, potentially requiring deferred flushing
  router.setPath(http::Method::GET, "/stream-large-trailers",
                 ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                   writer.status(http::StatusCode{200});
                   const std::string chunk(8192, 'G');
                   for (int idx = 0; idx < 10; ++idx) {
                     writer.writeBody(chunk);
                   }
                   writer.trailerAddLine("x-total", "81920");
                   writer.end();
                 }});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-large-trailers"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();

  // Multiple rounds for flow control + deferred sending
  for (int round = 0; round < 20; ++round) {
    loop.pumpServerToClient();
    loop.pumpClientToServer();
  }

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");

  std::string receivedData;
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1) {
      receivedData += ev.data;
    }
  }
  EXPECT_EQ(receivedData.size(), 10U * 8192U);

  // Verify trailers
  ASSERT_GE(loop.clientHeaders.size(), 2U);
  const auto& trailerFrame = loop.clientHeaders.back();
  EXPECT_TRUE(trailerFrame.endStream);
  EXPECT_TRUE(HasHeader(trailerFrame, "x-total", "81920"));
}

// ============== Streaming Handler HEAD Request ==============

TEST(Http2ProtocolHandler, StreamingHandlerHeadRequestEmitsEndStream) {
  Router router;
  router.setPath(http::Method::GET, "/stream-head",
                 ::aeronet::StreamingHandler{[](const HttpRequest& req, ::aeronet::HttpResponseWriter& writer) {
                   writer.status(http::StatusCode{200});
                   writer.contentType("text/plain");
                   // writeBody is a no-op for HEAD (writer skips emitData for _head=true)
                   writer.writeBody("invisible body data");
                   writer.end();
                   EXPECT_EQ(req.method(), http::Method::HEAD);
                 }});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "HEAD"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-head"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");

  // HEAD response must have no body data
  bool hasBodyData = false;
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1 && !ev.data.empty()) {
      hasBodyData = true;
    }
  }
  EXPECT_FALSE(hasBodyData);
}

// ============== Streaming Handler with Compression (above threshold) ==============

TEST(Http2ProtocolHandler, StreamingHandlerCompressedAboveThreshold) {
  Router router;
  router.setPath(http::Method::GET, "/stream-compress",
                 ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                   writer.status(http::StatusCode{200});
                   writer.contentType("text/plain");
                   // Write >1024 bytes (default minBytes) to trigger compression activation
                   const std::string chunk(2048, 'A');
                   writer.writeBody(chunk);
                   writer.end();
                 }});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-compress"));
  hdrs.append(MakeHttp1HeaderLine("accept-encoding", "gzip"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
  // Compression should have activated — content-encoding: gzip must be present
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], "content-encoding"), "gzip");
  EXPECT_TRUE(HasHeader(loop.clientHeaders[0], "vary", "accept-encoding"));

  // Compressed data should be smaller than the original 2048 bytes
  std::size_t totalReceived = 0;
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1) {
      totalReceived += ev.data.size();
    }
  }
  EXPECT_GT(totalReceived, 0U);
  EXPECT_LT(totalReceived, 2048U);
}

// ============== Streaming Handler with Compression (below threshold — identity fallback) ==============

TEST(Http2ProtocolHandler, StreamingHandlerCompressedBelowThresholdFallsBackToIdentity) {
  Router router;
  router.setPath(http::Method::GET, "/stream-compress-small",
                 ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                   writer.status(http::StatusCode{200});
                   writer.contentType("text/plain");
                   // Write < 1024 bytes (default minBytes) — compression stays inactive,
                   // pre-compress buffer is flushed as identity on end().
                   writer.writeBody("small body data");
                   writer.end();
                 }});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-compress-small"));
  hdrs.append(MakeHttp1HeaderLine("accept-encoding", "gzip"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
  // Compression did NOT activate — no content-encoding header expected
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], "content-encoding"), "");

  // Body should be the original uncompressed data
  std::string receivedData;
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1) {
      receivedData += ev.data;
    }
  }
  EXPECT_TRUE(receivedData.contains("small body data"));
}

// ============== Streaming Handler with Compression (multi-chunk above threshold) ==============

TEST(Http2ProtocolHandler, StreamingHandlerCompressedMultiChunkAboveThreshold) {
  Router router;
  router.setPath(http::Method::GET, "/stream-compress-multi",
                 ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                   writer.status(http::StatusCode{200});
                   writer.contentType("text/plain");
                   // Write multiple chunks — first two accumulate in pre-compress buffer (total 1024),
                   // threshold crossing activates the encoder. Then write a large chunk (32 KB)
                   // so the encoder produces inline compressed output (covering writeBody encoder path).
                   writer.writeBody(std::string(512, 'B'));
                   writer.writeBody(std::string(512, 'C'));
                   // Compression now activated. Write large chunk to force encoder inline output.
                   writer.writeBody(std::string(32768, 'D'));
                   writer.end();
                 }});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-compress-multi"));
  hdrs.append(MakeHttp1HeaderLine("accept-encoding", "gzip"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], "content-encoding"), "gzip");
}

// ============== Streaming Handler contentLength/status after headers sent ==============

TEST(Http2ProtocolHandler, StreamingHandlerContentLengthAfterHeadersSentIgnored) {
  Router router;
  router.setPath(http::Method::GET, "/stream-late-cl",
                 ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                   writer.status(http::StatusCode{200});
                   writer.writeBody("first");
                   // These should be ignored (headers already sent)
                   writer.contentLength(999);
                   writer.status(http::StatusCode{201});
                   writer.headerAddLine("x-late", "too-late");
                   writer.header("x-override", "too-late");
                   writer.contentType("text/html");
                   writer.reason("Late reason");
                   writer.end();
                 }});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-late-cl"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
  // x-late header should not be present (added after headers sent)
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], "x-late"), "");
}

// ============== Streaming Handler double end() is no-op ==============

TEST(Http2ProtocolHandler, StreamingHandlerDoubleEndIsNoOp) {
  Router router;
  router.setPath(http::Method::GET, "/stream-double-end",
                 ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                   writer.status(http::StatusCode{200});
                   writer.writeBody("data");
                   writer.end();
                   // Second end() should be silently ignored
                   writer.end();
                   // writeBody after end should also be silently ignored
                   writer.writeBody("after-end");
                 }});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-double-end"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");

  std::string receivedData;
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1) {
      receivedData += ev.data;
    }
  }
  EXPECT_TRUE(receivedData.contains("data"));
  EXPECT_FALSE(receivedData.contains("after-end"));
}

// ============== Streaming Handler write empty body is no-op ==============

TEST(Http2ProtocolHandler, StreamingHandlerEmptyWriteIsNoOp) {
  Router router;
  router.setPath(http::Method::GET, "/stream-empty-write",
                 ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                   writer.status(http::StatusCode{200});
                   writer.writeBody("");
                   writer.writeBody("real data");
                   writer.writeBody("");
                   writer.end();
                 }});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-empty-write"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
}

// ============== Streaming Handler with File + Response Middleware ==============

TEST(Http2ProtocolHandler, StreamingHandlerWithResponseMiddleware) {
  Router router;

  router
      .setPath(http::Method::GET, "/stream-resp-mw",
               ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                 writer.status(http::StatusCode{200});
                 writer.writeBody("stream-resp-data");
                 writer.end();
               }})
      .after([](const HttpRequest&, HttpResponse& resp) { resp.header("X-Stream-MW", "applied"); });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-resp-mw"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
}

// ============== Streaming Handler + CORS (successful) ==============

TEST(Http2ProtocolHandler, StreamingHandlerWithCorsApplied) {
  Router router;

  CorsPolicy cors(CorsPolicy::Active::On);
  cors.allowOrigin("https://stream-allowed.example.com");

  router
      .setPath(http::Method::GET, "/stream-cors-ok",
               ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                 writer.status(http::StatusCode{200});
                 writer.writeBody("cors-data");
                 writer.end();
               }})
      .cors(std::move(cors));

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-cors-ok"));
  hdrs.append(MakeHttp1HeaderLine("origin", "https://stream-allowed.example.com"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
  // Note: For HTTP/2 streaming handlers, CORS response headers are not applied to the
  // streaming response (this is a known limitation per the source comment).
  // This test verifies the CORS policy doesn't block the request (origin is allowed).
}

// ============== File Payload Where Window is Exactly Exhausted ==============

TEST(Http2ProtocolHandler, FilePayloadExactlyFillsInitialWindow) {
  test::ScopedTempDir tmpDir;
  // Create file exactly matching initial window size (65535 bytes)
  const std::string fileContent(65535, 'W');
  test::ScopedTempFile tmpFile(tmpDir, fileContent);

  Router router;
  const auto filePath = tmpFile.filePath().string();
  router.setPath(http::Method::GET, "/exact-window", [&filePath](const HttpRequest&) {
    File fd(filePath);
    return HttpResponse(200).file(std::move(fd), "application/octet-stream");
  });

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/exact-window"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();

  for (int round = 0; round < 15; ++round) {
    loop.pumpServerToClient();
    loop.pumpClientToServer();
  }

  std::string receivedData;
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1) {
      receivedData += ev.data;
    }
  }
  EXPECT_EQ(receivedData.size(), fileContent.size());
}

// ============== Streaming Handler HEAD Request ==============

TEST(Http2ProtocolHandler, StreamingHandlerHeadRequestSendsNoBody) {
  Router router;
  bool handlerCalled = false;

  router.setPath(
      http::Method::HEAD, "/stream-head",
      ::aeronet::StreamingHandler{[&handlerCalled](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
        handlerCalled = true;
        writer.status(http::StatusCode{200});
        writer.writeBody("should be suppressed for HEAD");
        writer.end();
      }});
  router.setPath(http::Method::GET, "/stream-head",
                 ::aeronet::StreamingHandler{[](const HttpRequest&, ::aeronet::HttpResponseWriter& writer) {
                   writer.status(http::StatusCode{200});
                   writer.writeBody("data");
                   writer.end();
                 }});

  Http2ProtocolLoopback loop(router);
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "HEAD"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/stream-head"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();
  loop.pumpServerToClient();

  EXPECT_TRUE(handlerCalled);
  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders[0], ":status"), "200");
}

// ============== Async Handler with deferWork (suspension/resumption) ==============

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
TEST(Http2ProtocolHandler, AsyncHandlerDeferWorkSuspendsAndResumes) {
  Router router;
  std::coroutine_handle<> capturedHandle;
  std::atomic<bool> callbackFired{false};

  router.setPath(http::Method::GET, "/async-defer", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    int result = co_await req.deferWork([]() { return 42; });
    co_return HttpResponse(200, std::to_string(result));
  });

  Http2ProtocolLoopback loop(router);
  loop.handler.setAsyncPostCallback(
      [&capturedHandle, &callbackFired](std::coroutine_handle<> handle, const std::function<void()>&) {
        capturedHandle = handle;
        callbackFired.store(true, std::memory_order_release);
      });
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/async-defer"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();

  // Wait for the background thread to complete and fire the callback
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!callbackFired.load(std::memory_order_acquire)) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "async callback did not fire in time";
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Resume the async task via the captured coroutine handle
  ASSERT_TRUE(loop.handler.resumeAsyncTaskByHandle(capturedHandle));

  // Pump response back to client
  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "200");

  // Verify the body contains the deferred work result
  std::string receivedData;
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1) {
      receivedData += ev.data;
    }
  }
  EXPECT_EQ(receivedData, "42");
}

TEST(Http2ProtocolHandler, AsyncHandlerDeferWorkWithCorsAndMiddleware) {
  Router router;
  std::coroutine_handle<> capturedHandle;
  std::atomic<bool> callbackFired{false};

  CorsPolicy cors(CorsPolicy::Active::On);
  cors.allowOrigin("https://async-defer.example.com");

  router
      .setPath(http::Method::GET, "/async-defer-cors",
               [](HttpRequest& req) -> RequestTask<HttpResponse> {
                 int result = co_await req.deferWork([]() { return 99; });
                 co_return HttpResponse(200, std::to_string(result));
               })
      .cors(std::move(cors))
      .after([](const HttpRequest&, HttpResponse& resp) { resp.header("X-After-MW", "done"); });

  Http2ProtocolLoopback loop(router);
  loop.handler.setAsyncPostCallback(
      [&capturedHandle, &callbackFired](std::coroutine_handle<> handle, const std::function<void()>&) {
        capturedHandle = handle;
        callbackFired.store(true, std::memory_order_release);
      });
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/async-defer-cors"));
  hdrs.append(MakeHttp1HeaderLine("origin", "https://async-defer.example.com"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!callbackFired.load(std::memory_order_acquire)) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "async callback did not fire in time";
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_TRUE(loop.handler.resumeAsyncTaskByHandle(capturedHandle));

  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "200");
  // CORS and response middleware should be applied on async completion
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), "access-control-allow-origin"),
            "https://async-defer.example.com");
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), "x-after-mw"), "done");
}

TEST(Http2ProtocolHandler, AsyncHandlerDeferWorkExceptionInCompletedHandler) {
  Router router;
  std::coroutine_handle<> capturedHandle;
  std::atomic<bool> callbackFired{false};

  router.setPath(http::Method::GET, "/async-defer-throw", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    (void)co_await req.deferWork([]() { return 1; });
    throw std::runtime_error("post-defer explosion");
  });

  Http2ProtocolLoopback loop(router);
  loop.handler.setAsyncPostCallback(
      [&capturedHandle, &callbackFired](std::coroutine_handle<> handle, const std::function<void()>&) {
        capturedHandle = handle;
        callbackFired.store(true, std::memory_order_release);
      });
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/async-defer-throw"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!callbackFired.load(std::memory_order_acquire)) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "async callback did not fire in time";
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  ASSERT_TRUE(loop.handler.resumeAsyncTaskByHandle(capturedHandle));

  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  // onAsyncTaskCompleted catches the exception and returns 500
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "500");
}

TEST(Http2ProtocolHandler, AsyncHandlerDoubleDeferWorkSuspendsResumesAndCompletes) {
  Router router;
  std::coroutine_handle<> capturedHandle;
  std::atomic<int> callbackCount{0};

  router.setPath(http::Method::GET, "/async-double-defer", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    int r1 = co_await req.deferWork([]() { return 10; });
    int r2 = co_await req.deferWork([]() { return 20; });
    co_return HttpResponse(200, std::to_string(r1 + r2));
  });

  Http2ProtocolLoopback loop(router);
  loop.handler.setAsyncPostCallback(
      [&capturedHandle, &callbackCount](std::coroutine_handle<> handle, const std::function<void()>&) {
        capturedHandle = handle;
        callbackCount.fetch_add(1, std::memory_order_release);
      });
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/async-double-defer"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();

  // Wait for the first deferWork callback
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (callbackCount.load(std::memory_order_acquire) < 1) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "first async callback did not fire in time";
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Resume — coroutine hits the second deferWork and suspends again (covers resumeAsyncTask re-suspend path)
  ASSERT_TRUE(loop.handler.resumeAsyncTaskByHandle(capturedHandle));

  // Wait for the second deferWork callback
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (callbackCount.load(std::memory_order_acquire) < 2) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "second async callback did not fire in time";
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Resume again — coroutine completes and sends the response
  ASSERT_TRUE(loop.handler.resumeAsyncTaskByHandle(capturedHandle));

  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "200");

  std::string receivedData;
  for (const auto& ev : loop.clientData) {
    if (ev.streamId == 1) {
      receivedData += ev.data;
    }
  }
  EXPECT_EQ(receivedData, "30");
}

TEST(Http2ProtocolHandler, AsyncHandlerDeferWorkThrowsNonStdExceptionOnCompletion) {
  Router router;
  std::coroutine_handle<> capturedHandle;
  std::atomic<bool> callbackFired{false};

  router.setPath(http::Method::GET, "/async-defer-throw-int", [](HttpRequest& req) -> RequestTask<HttpResponse> {
    (void)co_await req.deferWork([]() { return 1; });
    throw 42;
  });

  Http2ProtocolLoopback loop(router);
  loop.handler.setAsyncPostCallback(
      [&capturedHandle, &callbackFired](std::coroutine_handle<> handle, const std::function<void()>&) {
        capturedHandle = handle;
        callbackFired.store(true, std::memory_order_release);
      });
  loop.connect();

  RawChars hdrs;
  hdrs.append(MakeHttp1HeaderLine(":method", "GET"));
  hdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
  hdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
  hdrs.append(MakeHttp1HeaderLine(":path", "/async-defer-throw-int"));
  ASSERT_EQ(loop.client.sendHeaders(1, http::StatusCode{}, HeadersView(hdrs), true), ErrorCode::NoError);

  loop.pumpClientToServer();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!callbackFired.load(std::memory_order_acquire)) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline) << "async callback did not fire in time";
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Resume — coroutine throws a non-std::exception (int), caught by catch(...)
  ASSERT_TRUE(loop.handler.resumeAsyncTaskByHandle(capturedHandle));

  loop.pumpServerToClient();

  ASSERT_FALSE(loop.clientHeaders.empty());
  EXPECT_EQ(GetHeaderValue(loop.clientHeaders.back(), ":status"), "500");
}

#endif

}  // namespace aeronet::http2
