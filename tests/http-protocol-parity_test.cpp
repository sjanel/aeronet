// The request pipeline scenarios run over HTTP/1.1 and over cleartext HTTP/2 (prior knowledge), through the aeronet
// HttpClient: both protocols must answer them the same way.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/client-protocol.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/test_server_fixture.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/request-task.hpp"
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/websocket-endpoint.hpp"
#include "aeronet/websocket-handler.hpp"
#endif

namespace aeronet {
namespace {

constexpr std::string_view kAllowedOrigin = "https://app.example";
constexpr std::string_view kDeniedOrigin = "https://evil.example";

HttpServerConfig MakeServerConfig() {
  HttpServerConfig cfg;
  cfg.withPollInterval(std::chrono::milliseconds{5});
  return cfg;
}

CorsPolicy MakeCorsPolicy() {
  CorsPolicy policy(CorsPolicy::Active::On);
  policy.allowOrigin(kAllowedOrigin).allowMethods(http::Method::GET | http::Method::POST);
  return policy;
}

// The response middleware tagging every response, to check that a response went through it.
void TagResponse(const HttpRequestView& /*request*/, HttpResponse& response) { response.headerAddLine("x-post", "1"); }

class ProtocolParity : public ::testing::TestWithParam<HttpVersionMode> {
 protected:
  explicit ProtocolParity(HttpServerConfig cfg = MakeServerConfig(), RouterConfig routerCfg = {})
      : ts(std::move(cfg), std::move(routerCfg)) {}

  [[nodiscard]] static HttpClient MakeClient() {
    HttpClientConfig cfg;
    cfg.withHttpVersion(GetParam());
    cfg.followRedirects = false;
    return HttpClient(std::move(cfg));
  }

  [[nodiscard]] std::string url(std::string_view target) const {
    return std::format("http://127.0.0.1:{}{}", ts.port(), target);
  }

  // Sends the request built by 'build' from a client request to 'target' and returns the response.
  template <class Build>
  [[nodiscard]] HttpResponse send(http::Method method, std::string_view target, Build&& build) {
    HttpClient client = MakeClient();
    auto request = client.makeRequest(method, url(target));
    std::forward<Build>(build)(request);
    auto result = client.request(std::move(request));
    if (!result) {
      throw std::runtime_error(std::format("request to {} failed", target));
    }
    return std::move(result).value();
  }

  [[nodiscard]] HttpResponse send(http::Method method, std::string_view target) {
    return send(method, target, [](auto&) {});
  }

  test::TestServer ts;
};

TEST_P(ProtocolParity, NotFound) {
  ts.router().addResponseMiddleware(TagResponse);
  const auto resp = send(http::Method::GET, "/missing");
  EXPECT_EQ(resp.status(), http::StatusCodeNotFound);
  EXPECT_EQ(resp.headerValueOrEmpty("x-post"), "1");
}

#ifdef AERONET_ENABLE_WEBSOCKET
// A GET without upgrade to a WebSocket endpoint: 426 with Upgrade over HTTP/1.1 (RFC 9110 §15.5.22), 404 over HTTP/2,
// which forbids the Upgrade header (RFC 9113 §8.2.2) and does not support WebSocket. Both go through the response
// middleware.
TEST_P(ProtocolParity, WebSocketEndpointWithoutUpgrade) {
  ts.postRouterUpdate([](Router& router) {
    router.addResponseMiddleware(TagResponse);
    router.setWebSocket("/ws", WebSocketEndpoint::WithCallbacks(websocket::WebSocketCallbacks{}));
  });
  const auto resp = send(http::Method::GET, "/ws");
  if (GetParam() == HttpVersionMode::Http2) {
    EXPECT_EQ(resp.status(), http::StatusCodeNotFound);
  } else {
    EXPECT_EQ(resp.status(), http::StatusCodeUpgradeRequired);
    EXPECT_EQ(resp.headerValueOrEmpty(http::Upgrade), "websocket");
  }
  EXPECT_EQ(resp.headerValueOrEmpty("x-post"), "1");
}
#endif

TEST_P(ProtocolParity, MethodNotAllowed) {
  ts.router().addResponseMiddleware(TagResponse);
  ts.router().setPath(http::Method::GET, "/get-only", [](const HttpRequestView&) { return HttpResponse("ok"); });
  const auto resp = send(http::Method::POST, "/get-only", [](auto& req) { req.body("data"); });
  EXPECT_EQ(resp.status(), http::StatusCodeMethodNotAllowed);
  EXPECT_EQ(resp.bodyInMemory(), http::ReasonMethodNotAllowed);
  EXPECT_EQ(resp.headerValueOrEmpty(http::Allow), "GET");
  EXPECT_EQ(resp.headerValueOrEmpty("x-post"), "1");
}

// The Allow header of a 405 lists the methods of the route, whatever the kind of their handler (RFC 9110 §15.5.6).
TEST_P(ProtocolParity, MethodNotAllowedListsTheMethodsOfTheRoute) {
  ts.router().setPath(http::Method::GET | http::Method::PATCH, "/items/{id}",
                      [](const HttpRequestView&) { return HttpResponse("ok"); });
  ts.router().setPath(http::Method::PUT, "/items/{id}", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.writeBody("ok");
    writer.end();
  });
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  ts.router().setPath(http::Method::DELETE, "/items/{id}",
                      [](HttpRequestView&) -> RequestTask<HttpResponse> { co_return HttpResponse("ok"); });
  static constexpr std::string_view kAllow = "GET, PUT, DELETE, PATCH";
#else
  static constexpr std::string_view kAllow = "GET, PUT, PATCH";
#endif
  const auto resp = send(http::Method::POST, "/items/42", [](auto& req) { req.body("data"); });
  EXPECT_EQ(resp.status(), http::StatusCodeMethodNotAllowed);
  EXPECT_EQ(resp.bodyInMemory(), http::ReasonMethodNotAllowed);
  EXPECT_EQ(resp.headerValueOrEmpty(http::Allow), kAllow);
}

// TRACE, disabled by default, is answered 405 with the methods of its route, without TRACE: a TRACE handler is not
// called.
TEST_P(ProtocolParity, DisabledTraceListsTheMethodsOfTheRoute) {
  std::atomic<int> traceCalls{0};
  ts.router().setPath(http::Method::GET | http::Method::POST, "/res",
                      [](const HttpRequestView&) { return HttpResponse("ok"); });
  ts.router().setPath(http::Method::TRACE, "/res", [&traceCalls](const HttpRequestView&) {
    ++traceCalls;
    return HttpResponse("trace");
  });
  const auto resp = send(http::Method::TRACE, "/res");
  EXPECT_EQ(resp.status(), http::StatusCodeMethodNotAllowed);
  EXPECT_EQ(resp.bodyInMemory(), http::ReasonMethodNotAllowed);
  EXPECT_EQ(resp.headerValueOrEmpty(http::Allow), "GET, POST");
  EXPECT_EQ(traceCalls.load(), 0);

  // A target without route supports no method: its Allow header is empty (RFC 9110 §10.2.1).
  const auto missing = send(http::Method::TRACE, "/missing");
  EXPECT_EQ(missing.status(), http::StatusCodeMethodNotAllowed);
  EXPECT_EQ(missing.headerValue(http::Allow), std::optional<std::string_view>(""));
}

TEST_P(ProtocolParity, HandlerExceptionAnswers500ThroughResponseMiddleware) {
  ts.router().addResponseMiddleware(TagResponse);
  ts.router().setPath(http::Method::GET, "/throw",
                      [](const HttpRequestView&) -> HttpResponse { throw std::runtime_error("boom"); });
  const auto resp = send(http::Method::GET, "/throw");
  EXPECT_EQ(resp.status(), http::StatusCodeInternalServerError);
  EXPECT_EQ(resp.headerValueOrEmpty("x-post"), "1");
}

TEST_P(ProtocolParity, RequestMiddlewareSeesPathAndQueryParameters) {
  std::mutex mutex;
  std::string seen;
  ts.router().addRequestMiddleware([&](HttpRequestView& req) {
    std::scoped_lock lock(mutex);
    seen = std::format("{}|{}", req.pathParamValueOrEmpty("id"), req.queryParamValueOrEmpty("q"));
    return MiddlewareResult::Continue();
  });
  ts.router().setPath(http::Method::GET, "/items/{id}", [](const HttpRequestView& req) {
    return HttpResponse(std::format("{}|{}", req.pathParamValueOrEmpty("id"), req.queryParamValueOrEmpty("q")));
  });
  const auto resp = send(http::Method::GET, "/items/42?q=abc");
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.bodyInMemory(), "42|abc");
  std::scoped_lock lock(mutex);
  EXPECT_EQ(seen, "42|abc");
}

TEST_P(ProtocolParity, ShortCircuitGoesThroughResponseMiddleware) {
  std::atomic<int> handlerCalls{0};
  ts.router().addRequestMiddleware([](HttpRequestView&) {
    return MiddlewareResult::ShortCircuit(HttpResponse(http::StatusCodeUnauthorized).body("authentication required"));
  });
  ts.router().addResponseMiddleware(TagResponse);
  ts.router().setPath(http::Method::GET, "/secret", [&handlerCalls](const HttpRequestView&) {
    ++handlerCalls;
    return HttpResponse("secret");
  });
  const auto resp = send(http::Method::GET, "/secret");
  EXPECT_EQ(resp.status(), http::StatusCodeUnauthorized);
  EXPECT_EQ(resp.bodyInMemory(), "authentication required");
  EXPECT_EQ(resp.headerValueOrEmpty("x-post"), "1");
  EXPECT_EQ(handlerCalls.load(), 0);
}

TEST_P(ProtocolParity, MiddlewareMetricsCallbackFires) {
  std::atomic<int> nbPre{0};
  std::atomic<int> nbPost{0};
  ts.server.setMiddlewareMetricsCallback(
      [&](const MiddlewareMetrics& metrics) { ++(metrics.phase == MiddlewareMetrics::Phase::Pre ? nbPre : nbPost); });
  ts.router().addRequestMiddleware([](HttpRequestView&) { return MiddlewareResult::Continue(); });
  ts.router().addResponseMiddleware(TagResponse);
  ts.router()
      .setPath(http::Method::GET, "/metrics", [](const HttpRequestView&) { return HttpResponse("ok"); })
      .before([](HttpRequestView&) { return MiddlewareResult::Continue(); })
      .after(TagResponse);
  const auto resp = send(http::Method::GET, "/metrics");
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  // The callback runs before the response is sent.
  EXPECT_EQ(nbPre.load(), 2);
  EXPECT_EQ(nbPost.load(), 2);
}

TEST_P(ProtocolParity, CorsDeniedOriginDoesNotRunHandler) {
  std::atomic<int> handlerCalls{0};
  ts.router().addResponseMiddleware(TagResponse);
  ts.router()
      .setPath(http::Method::POST, "/cors",
               [&handlerCalls](const HttpRequestView&) {
                 ++handlerCalls;
                 return HttpResponse("side effect");
               })
      .cors(MakeCorsPolicy());
  const auto denied = send(http::Method::POST, "/cors", [](auto& req) {
    req.headerAddLine(http::Origin, kDeniedOrigin);
    req.body("data");
  });
  EXPECT_EQ(denied.status(), http::StatusCodeForbidden);
  EXPECT_EQ(denied.headerValueOrEmpty("x-post"), "1");
  EXPECT_EQ(handlerCalls.load(), 0);

  const auto allowed = send(http::Method::POST, "/cors", [](auto& req) {
    req.headerAddLine(http::Origin, kAllowedOrigin);
    req.body("data");
  });
  EXPECT_EQ(allowed.status(), http::StatusCodeOK);
  EXPECT_EQ(allowed.headerValueOrEmpty(http::AccessControlAllowOrigin), kAllowedOrigin);
  EXPECT_EQ(handlerCalls.load(), 1);
}

TEST_P(ProtocolParity, CorsDeniedOriginDoesNotRunStreamingHandler) {
  std::atomic<int> handlerCalls{0};
  ts.router()
      .setPath(http::Method::POST, "/cors-stream",
               [&handlerCalls](const HttpRequestView&, HttpResponseWriter& writer) {
                 ++handlerCalls;
                 writer.writeBody("side effect");
                 writer.end();
               })
      .cors(MakeCorsPolicy());
  const auto resp = send(http::Method::POST, "/cors-stream", [](auto& req) {
    req.headerAddLine(http::Origin, kDeniedOrigin);
    req.body("data");
  });
  EXPECT_EQ(resp.status(), http::StatusCodeForbidden);
  EXPECT_EQ(handlerCalls.load(), 0);
}

TEST_P(ProtocolParity, StreamingResponseGoesThroughResponseMiddlewareAndCors) {
  ts.router().addResponseMiddleware(TagResponse);
  ts.router()
      .setPath(http::Method::GET, "/stream",
               [](const HttpRequestView&, HttpResponseWriter& writer) {
                 writer.writeBody("alpha-");
                 writer.writeBody("beta");
                 writer.end();
               })
      .cors(MakeCorsPolicy());
  const auto resp =
      send(http::Method::GET, "/stream", [](auto& req) { req.headerAddLine(http::Origin, kAllowedOrigin); });
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.bodyInMemory(), "alpha-beta");
  EXPECT_EQ(resp.headerValueOrEmpty("x-post"), "1");
  EXPECT_EQ(resp.headerValueOrEmpty(http::AccessControlAllowOrigin), kAllowedOrigin);
}

// keep-alive and proxy-connection are not reserved: a handler may set them for HTTP/1.1, whereas they would make an
// HTTP/2 response malformed (RFC 9113 §8.2.2), which HttpClient rejects.
TEST_P(ProtocolParity, ConnectionSpecificResponseFieldsAreOnlySentOverHttp1) {
  ts.router().setPath(http::Method::GET, "/normal", [](const HttpRequestView& req) {
    return req.makeResponse(200)
        .headerAddLine("keep-alive", "timeout=5")
        .headerAddLine("proxy-connection", "keep-alive")
        .headerAddLine("x-kept", "1");
  });
  ts.router().setPath(http::Method::GET, "/streaming", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.headerAddLine("keep-alive", "timeout=5");
    writer.headerAddLine("proxy-connection", "keep-alive");
    writer.headerAddLine("x-kept", "1");
    writer.writeBody("body");
    writer.end();
  });

  const bool overHttp1 = GetParam() == HttpVersionMode::Http1_1;
  for (const std::string_view target : {"/normal", "/streaming"}) {
    const auto resp = send(http::Method::GET, target);
    EXPECT_EQ(resp.status(), http::StatusCodeOK) << target;
    EXPECT_EQ(resp.headerValueOrEmpty("x-kept"), "1") << target;
    EXPECT_EQ(resp.headerValueOrEmpty("keep-alive"), overHttp1 ? "timeout=5" : "") << target;
    EXPECT_EQ(resp.headerValueOrEmpty("proxy-connection"), overHttp1 ? "keep-alive" : "") << target;
  }
}

#ifdef AERONET_ENABLE_ZLIB
// A HEAD response carries the headers of the GET one (RFC 9110 §9.3.2), compression included. The client keeps no
// Content-Type nor Content-Length without a body: their values are checked on the wire by the tests of each protocol.
TEST_P(ProtocolParity, HeadCarriesTheHeadersOfTheCompressedGet) {
  static constexpr auto kGetHead = http::Method::GET | http::Method::HEAD;
  const std::string payload(4000, 'a');
  ts.router().setPath(kGetHead, "/head/plain", [payload](const HttpRequestView&) { return HttpResponse(payload); });
  ts.router().setPath(kGetHead, "/head/direct", [payload](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    resp.body(std::string_view(payload));  // direct compression
    return resp;
  });
  ts.router().setPath(kGetHead, "/head/streamed", [payload](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.writeBody(payload);
    writer.end();
  });

  // Without automatic decompression, the client keeps the headers and the body as received.
  HttpClientConfig cfg;
  cfg.withHttpVersion(GetParam());
  cfg.withDecompression(false).withDefaultAcceptEncoding("gzip");
  HttpClient client(std::move(cfg));
  for (std::string_view target : {"/head/plain", "/head/direct", "/head/streamed"}) {
    SCOPED_TRACE(target);
    auto get = client.request(client.makeRequest(http::Method::GET, url(target)));
    auto head = client.request(client.makeRequest(http::Method::HEAD, url(target)));
    ASSERT_TRUE(get.has_value() && head.has_value());
    EXPECT_EQ(get->status(), http::StatusCodeOK);
    EXPECT_EQ(head->status(), http::StatusCodeOK);
    EXPECT_EQ(get->headerValueOrEmpty(http::ContentEncoding), "gzip");
    EXPECT_LT(get->bodyInMemory().size(), payload.size());
    EXPECT_EQ(head->headerValueOrEmpty(http::ContentEncoding), "gzip");
    EXPECT_EQ(head->headerValueOrEmpty(http::Vary), get->headerValueOrEmpty(http::Vary));
    EXPECT_TRUE(head->bodyInMemory().empty());
  }
}
#endif

#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
// brotli and zstd write their output in bursts larger than the room given for one write of the handler: brotli (from
// quality 4) once per metablock, spanning many input blocks, zstd once per block. A streaming or directly compressed
// body of a few MiB of incompressible data used to fail: brotli stopped reading its input after about 1.5 MiB of 64 KiB
// writes, zstd after a block of a 1 MiB write.
class ProtocolParityFastBrotli : public ProtocolParity {
 protected:
  // The default brotli quality (11) is slow in Debug builds: quality 5 stalls the same way.
  ProtocolParityFastBrotli() : ProtocolParity(MakeFastBrotliServerConfig()) {}

  static HttpServerConfig MakeFastBrotliServerConfig() {
    HttpServerConfig cfg = MakeServerConfig();
    cfg.compression.brotli.quality = 5;
    return cfg;
  }
};

TEST_P(ProtocolParityFastBrotli, LargeIncompressibleBodiesAreCompressedWhole) {
  static constexpr std::size_t kPieceSize = 64UL << 10;
  static constexpr std::size_t kNbPieces = 32;
  static constexpr std::size_t kLargeSize = 1UL << 20;
  const RawChars payload = test::MakeRandomPayload((kPieceSize * kNbPieces) + kLargeSize);
  // kNbPieces pieces of kPieceSize bytes, then one of kLargeSize bytes.
  const auto forEachPiece = [](std::string_view body, auto&& func) {
    for (std::size_t pos = 0; pos < kPieceSize * kNbPieces; pos += kPieceSize) {
      func(body.substr(pos, kPieceSize));
    }
    func(body.substr(kPieceSize * kNbPieces));
  };

  ts.router().setPath(http::Method::GET, "/streamed",
                      [payload, forEachPiece](const HttpRequestView&, HttpResponseWriter& writer) {
                        forEachPiece(payload, [&writer](std::string_view piece) { writer.writeBody(piece); });
                        writer.end();
                      });
  ts.router().setPath(http::Method::GET, "/appended", [payload, forEachPiece](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    forEachPiece(payload, [&resp](std::string_view piece) { resp.bodyAppend(piece); });  // the first sets the body
    return resp;
  });
  ts.router().setPath(http::Method::GET, "/inline-appended", [payload, forEachPiece](const HttpRequestView& req) {
    auto resp = req.makeResponse();
    bool first = true;
    forEachPiece(payload, [&resp, &first](std::string_view piece) {
      if (first) {
        resp.body(piece);  // starts the direct compression, that bodyInlineAppend() continues
        first = false;
      } else {
        resp.bodyInlineAppend(piece.size(), [piece](char* out) {
          std::memcpy(out, piece.data(), piece.size());
          return piece.size();
        });
      }
    });
    return resp;
  });

  for (const Encoding encoding : test::SupportedEncodings()) {
    // Without automatic decompression, the client keeps the body as received.
    HttpClientConfig cfg;
    cfg.withHttpVersion(GetParam());
    cfg.withDecompression(false).withDefaultAcceptEncoding(GetEncodingStr(encoding));
    HttpClient client(std::move(cfg));
    for (std::string_view target : {"/streamed", "/appended", "/inline-appended"}) {
      SCOPED_TRACE(std::format("{} {}", GetEncodingStr(encoding), target));
      const auto resp = client.request(client.makeRequest(http::Method::GET, url(target)));
      ASSERT_TRUE(resp.has_value());
      EXPECT_EQ(resp->status(), http::StatusCodeOK);
      EXPECT_EQ(resp->headerValueOrEmpty(http::ContentEncoding), GetEncodingStr(encoding));
      EXPECT_EQ(std::string_view(test::Decompress(encoding, resp->bodyInMemory())), std::string_view(payload));
    }
  }
}
#endif

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
// 1xx, 204 and 304 responses carry no content, whatever the handler set: no body, no Content-Length, no Content-Type.
TEST_P(ProtocolParity, StatusesWithoutContentDropTheBody) {
  for (http::StatusCode status : {http::StatusCodeNoContent, http::StatusCodeNotModified}) {
    ts.router().setPath(http::Method::GET, std::format("/buffered{}", status), [status](const HttpRequestView&) {
      HttpResponse resp(status, "BODY-OF-RESPONSE");
      resp.trailerAddLine("x-trailer", "t").headerAddLine("x-custom", "kept");
      return resp;
    });
    ts.router().setPath(http::Method::GET, std::format("/streamed{}", status),
                        [status](const HttpRequestView&, HttpResponseWriter& writer) {
                          writer.status(status);
                          writer.headerAddLine("x-custom", "kept");
                          writer.writeBody("STREAMED-BODY-OF-RESPONSE");
                          writer.trailerAddLine("x-trailer", "t");
                          writer.end();
                        });
    for (std::string_view kind : {"/buffered", "/streamed"}) {
      SCOPED_TRACE(std::format("{}{}", kind, status));
      const auto resp = send(http::Method::GET, std::format("{}{}", kind, status));
      EXPECT_EQ(resp.status(), status);
      EXPECT_TRUE(resp.bodyInMemory().empty());
      EXPECT_EQ(resp.trailersSize(), 0U);
      EXPECT_EQ(resp.headerValueOrEmpty("x-custom"), "kept");
      EXPECT_FALSE(resp.headerValue(http::ContentLength).has_value());
      EXPECT_FALSE(resp.headerValue(http::TransferEncoding).has_value());
      EXPECT_FALSE(resp.headerValue(http::ContentType).has_value());
    }
  }
}

TEST_P(ProtocolParity, CorsDeniedOriginDoesNotRunAsyncHandler) {
  std::atomic<int> handlerCalls{0};
  ts.router()
      .setPath(http::Method::POST, "/cors-async",
               [&handlerCalls](HttpRequestView&) -> RequestTask<HttpResponse> {
                 ++handlerCalls;
                 co_return HttpResponse("side effect");
               })
      .cors(MakeCorsPolicy());
  const auto resp = send(http::Method::POST, "/cors-async", [](auto& req) {
    req.headerAddLine(http::Origin, kDeniedOrigin);
    req.body("data");
  });
  EXPECT_EQ(resp.status(), http::StatusCodeForbidden);
  EXPECT_EQ(handlerCalls.load(), 0);
}

TEST_P(ProtocolParity, AsyncHandlerExceptionAnswers500ThroughResponseMiddleware) {
  ts.router().addResponseMiddleware(TagResponse);
  // A coroutine throwing from its body.
  ts.router().setPath(http::Method::GET, "/async-throw", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    if (!req.path().empty()) {
      throw std::runtime_error("boom");
    }
    co_return HttpResponse("unreachable");
  });
  // A function returning a RequestTask without being a coroutine, throwing when called.
  ts.router().setPath(http::Method::GET, "/async-throw-on-call",
                      [](HttpRequestView&) -> RequestTask<HttpResponse> { throw std::runtime_error("boom"); });
  for (const std::string_view target : {"/async-throw", "/async-throw-on-call"}) {
    const auto resp = send(http::Method::GET, target);
    EXPECT_EQ(resp.status(), http::StatusCodeInternalServerError) << target;
    EXPECT_EQ(resp.headerValueOrEmpty("x-post"), "1") << target;
  }
  // The server still serves requests.
  EXPECT_EQ(send(http::Method::GET, "/missing").status(), http::StatusCodeNotFound);
}

#ifdef AERONET_ENABLE_ZLIB
TEST_P(ProtocolParity, AsyncHandlerReadsACompressedBodyInChunks) {
  ts.router().setPath(http::Method::POST, "/chunks", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    std::string received;
    while (req.hasMoreBody()) {
      const std::string_view chunk = co_await req.readBodyAsync(64);
      received.append(chunk);
    }
    co_return HttpResponse(std::move(received));
  });

  const std::string payload(300, 'z');
  const RawChars compressed = test::Compress(Encoding::gzip, payload);
  const auto resp = send(http::Method::POST, "/chunks", [&compressed](auto& req) {
    req.contentEncoding("gzip");
    req.body(std::string_view(compressed));
  });
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
  EXPECT_EQ(resp.bodyInMemory(), payload);
}
#endif
#endif

TEST_P(ProtocolParity, CorsPreflight) {
  ts.router()
      .setPath(http::Method::POST, "/cors", [](const HttpRequestView&) { return HttpResponse("ok"); })
      .cors(MakeCorsPolicy());
  const auto resp = send(http::Method::OPTIONS, "/cors", [](auto& req) {
    req.headerAddLine(http::Origin, kAllowedOrigin);
    req.headerAddLine(http::AccessControlRequestMethod, "POST");
  });
  EXPECT_EQ(resp.status(), http::StatusCodeNoContent);
  EXPECT_EQ(resp.headerValueOrEmpty(http::AccessControlAllowOrigin), kAllowedOrigin);
}

TEST_P(ProtocolParity, CorsPreflightOfAMethodOfNoHandlerIsAnswered405WithAllow) {
  ts.router()
      .setPath(http::Method::GET | http::Method::POST, "/cors",
               [](const HttpRequestView&) { return HttpResponse("ok"); })
      .cors(MakeCorsPolicy());
  const auto resp = send(http::Method::OPTIONS, "/cors", [](auto& req) {
    req.headerAddLine(http::Origin, kAllowedOrigin);
    req.headerAddLine(http::AccessControlRequestMethod, "PUT");
  });
  EXPECT_EQ(resp.status(), http::StatusCodeMethodNotAllowed);
  EXPECT_EQ(resp.bodyInMemory(), http::ReasonMethodNotAllowed);
  EXPECT_EQ(resp.headerValueOrEmpty(http::Allow), "GET, POST");
}

class ProtocolParityTrailingSlashRedirect : public ProtocolParity {
 protected:
  ProtocolParityTrailingSlashRedirect()
      : ProtocolParity(MakeServerConfig(),
                       RouterConfig{}.withTrailingSlashPolicy(RouterConfig::TrailingSlashPolicy::Redirect)) {}
};

TEST_P(ProtocolParityTrailingSlashRedirect, RedirectsToCanonicalPath) {
  ts.router().addResponseMiddleware(TagResponse);
  ts.router().setPath(http::Method::GET, "/foo", [](const HttpRequestView&) { return HttpResponse("foo"); });
  ts.router().setPath(http::Method::GET, "/bar/", [](const HttpRequestView&) { return HttpResponse("bar"); });

  const auto removeSlash = send(http::Method::GET, "/foo/");
  EXPECT_EQ(removeSlash.status(), http::StatusCodeMovedPermanently);
  EXPECT_EQ(removeSlash.headerValueOrEmpty(http::Location), "/foo");
  EXPECT_EQ(removeSlash.headerValueOrEmpty("x-post"), "1");

  const auto addSlash = send(http::Method::GET, "/bar");
  EXPECT_EQ(addSlash.status(), http::StatusCodeMovedPermanently);
  EXPECT_EQ(addSlash.headerValueOrEmpty(http::Location), "/bar/");

  EXPECT_EQ(send(http::Method::GET, "/foo").bodyInMemory(), "foo");
}

constexpr uint16_t kHttpsPort = 8443;

class ProtocolParityHttpsRedirect : public ProtocolParity {
 protected:
  ProtocolParityHttpsRedirect() : ProtocolParity(MakeServerConfig().withHttpsRedirect(kHttpsPort)) {}
};

TEST_P(ProtocolParityHttpsRedirect, PlaintextRequestsAreRedirected) {
  std::atomic<int> handlerCalls{0};
  ts.router().setPath(http::Method::GET | http::Method::POST, "/page", [&handlerCalls](const HttpRequestView&) {
    ++handlerCalls;
    return HttpResponse("served in cleartext");
  });

  const auto resp = send(http::Method::GET, "/page?a=1&b=two%20words");
  EXPECT_EQ(resp.status(), http::StatusCodeMovedPermanently);
  EXPECT_EQ(resp.headerValueOrEmpty(http::Location),
            std::format("https://127.0.0.1:{}/page?a=1&b=two%20words", kHttpsPort));

  // The body of the request is not read.
  const auto post = send(http::Method::POST, "/page", [](auto& req) { req.body(std::string(256UL * 1024UL, 'b')); });
  EXPECT_EQ(post.status(), http::StatusCodeMovedPermanently);
  EXPECT_EQ(handlerCalls.load(), 0);
}

std::string ProtocolName([[maybe_unused]] const ::testing::TestParamInfo<HttpVersionMode>& info) {
#ifdef AERONET_ENABLE_HTTP2
  return {info.param == HttpVersionMode::Http2 ? "Http2" : "Http1"};
#else
  return {"Http1"};
#endif
}

INSTANTIATE_TEST_SUITE_P(Protocols, ProtocolParity, ::testing::Values(HttpVersionMode::Http1_1, HttpVersionMode::Http2),
                         ProtocolName);
INSTANTIATE_TEST_SUITE_P(Protocols, ProtocolParityTrailingSlashRedirect,
                         ::testing::Values(HttpVersionMode::Http1_1, HttpVersionMode::Http2), ProtocolName);
INSTANTIATE_TEST_SUITE_P(Protocols, ProtocolParityHttpsRedirect,
                         ::testing::Values(HttpVersionMode::Http1_1, HttpVersionMode::Http2), ProtocolName);
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
INSTANTIATE_TEST_SUITE_P(Protocols, ProtocolParityFastBrotli,
                         ::testing::Values(HttpVersionMode::Http1_1, HttpVersionMode::Http2), ProtocolName);
#endif

}  // namespace
}  // namespace aeronet
