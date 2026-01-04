#include "aeronet/http2-protocol-handler.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/connection-state.hpp"
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
#include "aeronet/path-handler-entry.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/request-task.hpp"
#include "aeronet/router.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::http2 {
namespace {

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

class Http2ProtocolLoopback {
 public:
  explicit Http2ProtocolLoopback(Router& router)
      : compressionState(serverConfig.compression),
        handler(serverCfg, router, serverConfig, compressionState),
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

  router.setDefault([&handlerCalled](const HttpRequest& /*req*/) {
    handlerCalled = true;
    return HttpResponse(200);
  });

  auto handler = CreateHttp2ProtocolHandler(config, router, serverConfig, compressionState);

  ASSERT_NE(handler, nullptr);
  EXPECT_EQ(handler->type(), ProtocolType::Http2);
  EXPECT_FALSE(handlerCalled);
}

TEST(Http2ProtocolHandler, HasNoPendingOutputInitially) {
  Http2Config config;
  Router router;
  HttpServerConfig serverConfig;
  internal::ResponseCompressionState compressionState(serverConfig.compression);
  auto handler = CreateHttp2ProtocolHandler(config, router, serverConfig, compressionState);

  EXPECT_FALSE(handler->hasPendingOutput());
}

TEST(Http2ProtocolHandler, ConnectionPreface) {
  Http2Config config;
  Router router;
  HttpServerConfig serverConfig;
  internal::ResponseCompressionState compressionState(serverConfig.compression);
  auto handler = CreateHttp2ProtocolHandler(config, router, serverConfig, compressionState);

  EXPECT_FALSE(handler->hasPendingOutput());
}

TEST(Http2ProtocolHandler, InitiateClose) {
  Http2Config config;
  Router router;
  HttpServerConfig serverConfig;
  internal::ResponseCompressionState compressionState(serverConfig.compression);
  auto handler = CreateHttp2ProtocolHandler(config, router, serverConfig, compressionState);

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
  router.setDefault(
      [](const HttpRequest& req) { return HttpResponse().status(200).body("Hello from " + std::string(req.path())); });

  auto handler = CreateHttp2ProtocolHandler(config, router, serverConfig, compressionState);

  ASSERT_NE(handler, nullptr);
  EXPECT_EQ(handler->type(), ProtocolType::Http2);
}

TEST(CreateHttp2ProtocolHandler, SendServerPrefaceForTlsQueuesSettingsImmediately) {
  Http2Config config;
  Router router;
  router.setDefault([](const HttpRequest&) { return HttpResponse(200); });

  HttpServerConfig serverConfig;
  internal::ResponseCompressionState compressionState(serverConfig.compression);

  auto handlerBase = CreateHttp2ProtocolHandler(config, router, serverConfig, compressionState, true);
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
  Http2ProtocolHandler handler(config, router, serverConfig, compressionState);
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
  Http2ProtocolHandler original(config, router, serverConfig, compressionState);

  static_assert(noexcept(Http2ProtocolHandler(std::declval<Http2ProtocolHandler&&>())));
  static_assert(noexcept(std::declval<Http2ProtocolHandler&>() = std::declval<Http2ProtocolHandler&&>()));

  Http2ProtocolHandler moved(std::move(original));
  EXPECT_FALSE(moved.hasPendingOutput());

  Http2ProtocolHandler assigned(config, router, serverConfig, compressionState);
  assigned = std::move(moved);
  EXPECT_FALSE(assigned.hasPendingOutput());
}

TEST(Http2ProtocolHandler, SimpleGetWithBodyProducesHeadersAndData) {
  Router router;
  router.setPath(http::Method::GET, "/hello", [](const HttpRequest&) { return HttpResponse(200).body("abc"); });

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
                 [](const HttpRequest&) { return HttpResponse(200).body("abc").addTrailer("x-check", "ok"); });

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
                 [](const HttpRequest&) { return HttpResponse(200).addTrailer("x-check", "ok"); });

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
  EXPECT_TRUE(loop.clientData[0].data.contains("Trailers must be added after non empty body is set"));
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
  };

  const std::array<MethodCase, 9> cases = {
      MethodCase{1, "PUT", http::Method::PUT},      MethodCase{3, "DELETE", http::Method::DELETE},
      MethodCase{5, "HEAD", http::Method::HEAD},    MethodCase{7, "OPTIONS", http::Method::OPTIONS},
      MethodCase{9, "PATCH", http::Method::PATCH},  MethodCase{11, "CONNECT", http::Method::CONNECT},
      MethodCase{13, "TRACE", http::Method::TRACE}, MethodCase{15, "POST", http::Method::POST},
      MethodCase{17, "BREW", http::Method::GET},
  };

  for (const auto& tc : cases) {
    RawChars mhdrs;
    mhdrs.append(MakeHttp1HeaderLine(":method", std::string(tc.method)));
    mhdrs.append(MakeHttp1HeaderLine(":scheme", "https"));
    mhdrs.append(MakeHttp1HeaderLine(":authority", "example.com"));
    mhdrs.append(MakeHttp1HeaderLine(":path", "/m"));
    mhdrs.append(MakeHttp1HeaderLine(":unknown", "ignored"));
    const auto ok = loop.client.sendHeaders(tc.streamId, http::StatusCode{}, HeadersView(mhdrs), true);
    ASSERT_EQ(ok, ErrorCode::NoError);
    loop.pumpClientToServer();
    loop.pumpServerToClient();
  }

  ASSERT_EQ(seen.size(), cases.size());
  const auto caseCount = static_cast<decltype(seen)::size_type>(cases.size());
  for (decltype(seen)::size_type idx = 0; idx < caseCount; ++idx) {
    EXPECT_EQ(seen[idx], cases[idx].expected);
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

TEST(Http2ProtocolHandler, AsyncHandlerRunsToCompletion) {
  Router router;
  router.setPath(http::Method::GET, "/async",
                 [](HttpRequest&) -> RequestTask<HttpResponse> { co_return HttpResponse(200).body("async-ok"); });

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

}  // namespace
}  // namespace aeronet::http2
