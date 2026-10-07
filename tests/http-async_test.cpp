#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/request-metrics.hpp"
#include "aeronet/request-task.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"

#if defined(AERONET_ENABLE_HTTP2) && defined(AERONET_ENABLE_OPENSSL)
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/test_server_http2_tls_fixture.hpp"
#include "aeronet/test_tls_http2_client.hpp"
#endif

using namespace std::chrono_literals;

namespace aeronet {

namespace {

test::TestServer ts;

bool WaitFor(const std::atomic_bool& flag, std::chrono::milliseconds timeout = 5000ms) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!flag.load(std::memory_order_acquire)) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

// Sets a flag when destroyed: tells when the coroutine frame holding it is destroyed.
class DestructionFlag {
 public:
  explicit DestructionFlag(std::atomic_bool& flag) noexcept : _flag(&flag) {}

  DestructionFlag(const DestructionFlag&) = delete;
  DestructionFlag(DestructionFlag&&) noexcept = delete;
  DestructionFlag& operator=(const DestructionFlag&) = delete;
  DestructionFlag& operator=(DestructionFlag&&) noexcept = delete;

  ~DestructionFlag() { _flag->store(true, std::memory_order_release); }

 private:
  std::atomic_bool* _flag;
};

std::string PostHead(std::string_view target, std::size_t contentLength, std::string_view extraHeaders = {}) {
  std::string head("POST ");
  head.append(target).append(" HTTP/1.1\r\nHost: x\r\n").append(extraHeaders);
  head.append("Content-Length: ").append(std::to_string(contentLength)).append("\r\n\r\n");
  return head;
}

}  // namespace

// A response sent before the body of an async handler request was received (here by a short-circuiting request
// middleware) cannot consume the request: it used to be parsed and answered again and again.
TEST(HttpAsyncHandlers, MiddlewareShortCircuitBeforeBodyIsAnsweredOnce) {
  std::atomic_bool handlerCalled{false};
  ts.resetRouterAndGet()
      .setPath(http::Method::POST, "/upload",
               [&handlerCalled](HttpRequestView&) -> RequestTask<HttpResponse> {
                 handlerCalled.store(true);
                 co_return HttpResponse("handled");
               })
      .before([](HttpRequestView&) {
        return MiddlewareResult::ShortCircuit(HttpResponse(http::StatusCodeUnauthorized, "denied"));
      });

  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), PostHead("/upload", 100) + "partial");
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_EQ(test::countOccurrences(resp, "HTTP/1.1 "), 1) << resp.substr(0, 512);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 401")) << resp.substr(0, 512);
  EXPECT_FALSE(handlerCalled.load());
}

TEST(HttpAsyncHandlers, CorsRejectionBeforeBodyIsAnsweredOnce) {
  CorsPolicy policy;
  policy.allowOrigin("https://app.example").allowMethods(http::Method::POST);
  ts.resetRouterAndGet()
      .setPath(http::Method::POST, "/upload",
               [](HttpRequestView&) -> RequestTask<HttpResponse> { co_return HttpResponse("handled"); })
      .cors(std::move(policy));

  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), PostHead("/upload", 100, "Origin: https://evil.example\r\n") + "partial");
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_EQ(test::countOccurrences(resp, "HTTP/1.1 "), 1) << resp.substr(0, 512);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 403")) << resp.substr(0, 512);
}

TEST(HttpAsyncHandlers, PreflightBeforeBodyIsAnsweredOnce) {
  CorsPolicy policy;
  policy.allowOrigin("https://app.example").allowMethods(http::Method::POST).allowAnyRequestHeaders();
  ts.resetRouterAndGet()
      .setPath(http::Method::POST | http::Method::OPTIONS, "/upload",
               [](HttpRequestView&) -> RequestTask<HttpResponse> { co_return HttpResponse("handled"); })
      .cors(std::move(policy));

  test::ClientConnection client(ts.port());
  std::string request(
      "OPTIONS /upload HTTP/1.1\r\nHost: x\r\nOrigin: https://app.example\r\nAccess-Control-Request-Method: POST\r\n"
      "Content-Length: 100\r\n\r\npartial");
  test::sendAll(client.fd(), request);
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_EQ(test::countOccurrences(resp, "HTTP/1.1 "), 1) << resp.substr(0, 512);
}

// Query and path parameters point into the request head, copied out of the connection buffer when an async handler
// waits for its body: the buffer is reallocated when the (large) body arrives.
TEST(HttpAsyncHandlers, QueryAndPathParamsSurviveBodyArrival) {
  ts.resetRouterAndGet().setPath(
      http::Method::POST, "/items/{id}", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
        const std::string_view body = co_await req.bodyAwaitable();
        std::string result(req.queryParamValueOrEmpty("name"));
        result.append(":").append(req.pathParamValueOrEmpty("id")).append(":").append(std::to_string(body.size()));
        co_return HttpResponse(result);
      });

  static constexpr std::size_t kBodySize = std::size_t{1} << 20U;
  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), PostHead("/items/42?name=hello", kBodySize, "Connection: close\r\n"));
  std::this_thread::sleep_for(50ms);
  test::sendAll(client.fd(), std::string(kBodySize, 'x'), 5000ms);
  const std::string resp = test::recvUntilClosed(client.fd());
  EXPECT_TRUE(resp.ends_with("hello:42:1048576")) << resp.substr(0, 512);
}

// The body of a fixed-length request is a view into the connection buffer: it must survive the bytes received while
// the handler is suspended (here a large pipelined request), which are then served.
TEST(HttpAsyncHandlers, ContentLengthBodySurvivesBytesReceivedWhileSuspended) {
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/first", [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
    co_await req.deferWork([&] {
      started.store(true, std::memory_order_release);
      WaitFor(allow);
    });
    co_return HttpResponse(req.body());
  });
  ts.router().setPath(http::Method::POST, "/other", [](const HttpRequestView& req) {
    return HttpResponse("other:" + std::to_string(req.body().size()));
  });

  static constexpr std::size_t kPipelinedBody = std::size_t{1} << 20U;
  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), PostHead("/first", 5) + "hello");
  ASSERT_TRUE(WaitFor(started));
  // The server may not read these bytes before the handler completes: send them from another thread.
  std::jthread sender([fd = client.fd()] {
    test::sendAll(fd, PostHead("/other", kPipelinedBody, "Connection: close\r\n") + std::string(kPipelinedBody, 'y'),
                  10000ms);
  });
  std::this_thread::sleep_for(100ms);
  allow.store(true, std::memory_order_release);
  const std::string resp = test::recvUntilClosed(client.fd());
  sender.join();
  EXPECT_TRUE(resp.contains("\r\n\r\nhello")) << resp.substr(0, 512);
  EXPECT_TRUE(resp.ends_with("other:1048576")) << resp.substr(0, 512);
}

// Requests pipelined behind an async handler were only served when new bytes arrived.
TEST(HttpAsyncHandlers, PipelinedRequestAfterImmediatelyCompletingAsyncHandler) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/async", [](HttpRequestView&) -> RequestTask<HttpResponse> {
    co_return HttpResponse("async-done");
  });
  ts.router().setPath(http::Method::GET, "/sync", [](const HttpRequestView&) { return HttpResponse("sync-done"); });

  test::ClientConnection client(ts.port());
  const auto start = std::chrono::steady_clock::now();
  test::sendAll(client.fd(),
                "GET /async HTTP/1.1\r\nHost: x\r\n\r\nGET /sync HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  EXPECT_TRUE(resp.contains("async-done")) << resp;
  EXPECT_TRUE(resp.ends_with("sync-done")) << resp;
}

TEST(HttpAsyncHandlers, PipelinedRequestsAfterDeferredAsyncHandlers) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/async", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    const int value = co_await req.deferWork([] {
      std::this_thread::sleep_for(10ms);
      return 42;
    });
    co_return HttpResponse("async-" + std::to_string(value));
  });
  ts.router().setPath(http::Method::GET, "/sync", [](const HttpRequestView&) { return HttpResponse("sync-done"); });

  test::ClientConnection client(ts.port());
  const auto start = std::chrono::steady_clock::now();
  test::sendAll(client.fd(),
                "GET /async HTTP/1.1\r\nHost: x\r\n\r\nGET /async HTTP/1.1\r\nHost: x\r\n\r\n"
                "GET /sync HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_LT(std::chrono::steady_clock::now() - start, 1s);
  EXPECT_EQ(test::countOccurrences(resp, "async-42"), 2) << resp;
  EXPECT_TRUE(resp.ends_with("sync-done")) << resp;
}

// A request head is parsed again each time a part of its body is received: 100 Continue must be sent once.
TEST(HttpAsyncHandlers, ExpectContinueIsSentOnceForBodyReceivedInSeveralParts) {
  ts.resetRouterAndGet().setPath(http::Method::POST, "/sync", [](const HttpRequestView& req) {
    return HttpResponse("sync:" + std::to_string(req.body().size()));
  });
  ts.router().setPath(http::Method::POST, "/async", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    const std::string_view body = co_await req.bodyAwaitable();
    co_return HttpResponse("async:" + std::to_string(body.size()));
  });

  for (std::string_view target : {"/sync", "/async"}) {
    test::ClientConnection client(ts.port());
    test::sendAll(client.fd(), PostHead(target, 12, "Expect: 100-continue\r\nConnection: close\r\n"));
    for (std::string_view part : {"abcd", "efgh", "ijkl"}) {
      std::this_thread::sleep_for(20ms);
      test::sendAll(client.fd(), part);
    }
    const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
    EXPECT_EQ(test::countOccurrences(resp, "100 Continue"), 1) << resp;
    EXPECT_TRUE(resp.ends_with(":12")) << resp;
  }
}

// The router may be updated while an async handler is suspended: the handler (whose captures the coroutine uses), the
// names of its path parameters and the metadata of its route (response middleware, CORS) are destroyed or moved.
TEST(HttpAsyncHandlers, RouterUpdatesWhileHandlerIsSuspended) {
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  ts.resetRouterAndGet()
      .setPath(http::Method::GET, "/items/{id}",
               [captured = std::string(64, 'c'), &started, &allow](HttpRequestView& req) -> RequestTask<HttpResponse> {
                 co_await req.deferWork([&] {
                   started.store(true, std::memory_order_release);
                   WaitFor(allow);
                 });
                 std::string result(req.pathParamValueOrEmpty("id"));
                 result.append(":").append(std::to_string(captured.size()));
                 co_return HttpResponse(result);
               })
      .after([](const HttpRequestView&, HttpResponse& resp) { resp.headerAddLine("x-route", "original"); });

  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), "GET /items/42 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  ASSERT_TRUE(WaitFor(started));

  // Destroys the route (and its handler) of the suspended request, then reallocates the literal routes.
  ts.resetRouterAndGet()
      .setPath(http::Method::GET, "/items/{id}",
               [](HttpRequestView&) -> RequestTask<HttpResponse> { co_return HttpResponse("replaced"); })
      .after([](const HttpRequestView&, HttpResponse& resp) { resp.headerAddLine("x-route", "replaced"); });
  for (int idx = 0; idx < 64; ++idx) {
    ts.router().setPath(http::Method::GET, "/literal-" + std::to_string(idx),
                        [](HttpRequestView&) -> RequestTask<HttpResponse> { co_return HttpResponse("literal"); });
  }
  allow.store(true, std::memory_order_release);

  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_TRUE(resp.ends_with("42:64")) << resp;
  // The response middleware of the route as it is when the handler completes.
  EXPECT_TRUE(resp.contains("x-route: replaced")) << resp;
}

// A connection closed while deferred work runs (here by the per-route timeout) keeps the coroutine frame and the
// request until the work completes: the work may use them (references captured by the work function).
TEST(HttpAsyncHandlers, CoroutineFrameOutlivesItsClosedConnectionUntilWorkCompletes) {
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  std::atomic_bool frameDestroyed{false};
  std::atomic_bool frameAliveAtWorkEnd{false};
  ts.resetRouterAndGet()
      .setPath(http::Method::GET, "/slow",
               [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
                 DestructionFlag guard(frameDestroyed);
                 const std::string local = "local";
                 co_await req.deferWork([&] {
                   started.store(true, std::memory_order_release);
                   WaitFor(allow);
                   frameAliveAtWorkEnd.store(!frameDestroyed.load() && local == "local" && req.path() == "/slow");
                 });
                 co_return HttpResponse("done");
               })
      .timeout(50ms);

  {
    test::ClientConnection client(ts.port());
    test::sendAll(client.fd(), "GET /slow HTTP/1.1\r\nHost: x\r\n\r\n");
    ASSERT_TRUE(WaitFor(started));
    // The per-route timeout answers 408 and closes the connection while the work is still running.
    const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{5});
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 408")) << resp;
  }
  // Another connection may now reuse the state and the fd of the closed one.
  EXPECT_TRUE(test::simpleGet(ts.port(), "/unknown").starts_with("HTTP/1.1 404"));
  EXPECT_FALSE(frameDestroyed.load());

  allow.store(true, std::memory_order_release);
  EXPECT_TRUE(WaitFor(frameDestroyed));
  EXPECT_TRUE(frameAliveAtWorkEnd.load());
}

// Same when the request is abandoned because of an invalid body received while the work runs.
TEST(HttpAsyncHandlers, CoroutineFrameOfAbandonedRequestOutlivesItsWork) {
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  std::atomic_bool frameDestroyed{false};
  std::atomic_bool frameAliveAtWorkEnd{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/chunked",
                                 [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
                                   DestructionFlag guard(frameDestroyed);
                                   co_await req.deferWork([&] {
                                     started.store(true, std::memory_order_release);
                                     WaitFor(allow);
                                     frameAliveAtWorkEnd.store(!frameDestroyed.load() && req.path() == "/chunked");
                                   });
                                   const std::string_view body = co_await req.bodyAwaitable();
                                   co_return HttpResponse(body);
                                 });

  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), "POST /chunked HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nabcd\r\n");
  ASSERT_TRUE(WaitFor(started));
  test::sendAll(client.fd(), "zz\r\n");  // invalid chunk size
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
  EXPECT_FALSE(frameDestroyed.load());

  allow.store(true, std::memory_order_release);
  EXPECT_TRUE(WaitFor(frameDestroyed));
  EXPECT_TRUE(frameAliveAtWorkEnd.load());
}

// Without deferred work running, an abandoned request releases its coroutine right away.
TEST(HttpAsyncHandlers, InvalidBodyWhileWaitingForItReleasesTheCoroutine) {
  std::atomic_bool frameDestroyed{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/chunked",
                                 [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
                                   DestructionFlag guard(frameDestroyed);
                                   const std::string_view body = co_await req.bodyAwaitable();
                                   co_return HttpResponse(body);
                                 });

  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), "POST /chunked HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n4\r\nabcd\r\n");
  std::this_thread::sleep_for(20ms);
  test::sendAll(client.fd(), "zz\r\n");  // invalid chunk size
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 400")) << resp;
  EXPECT_TRUE(WaitFor(frameDestroyed));
}

#ifdef AERONET_ENABLE_ZLIB
TEST(HttpAsyncHandlers, UndecodableBodyReceivedAfterDispatchIsRejected) {
  std::atomic_bool frameDestroyed{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/gzip", [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
    DestructionFlag guard(frameDestroyed);
    const std::string_view body = co_await req.bodyAwaitable();
    co_return HttpResponse(body);
  });

  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), PostHead("/gzip", 10, "Content-Encoding: gzip\r\n"));
  std::this_thread::sleep_for(20ms);
  test::sendAll(client.fd(), "notgzipped");
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 4")) << resp;
  EXPECT_TRUE(WaitFor(frameDestroyed));
}
#endif

// The connection input is not read while an async handler runs: a client half-closing its connection meanwhile still
// gets the response.
TEST(HttpAsyncHandlers, HalfClosedClientGetsTheResponseOfARunningHandler) {
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  ts.resetRouterAndGet().setPath(http::Method::GET, "/slow", [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
    co_await req.deferWork([&] {
      started.store(true, std::memory_order_release);
      WaitFor(allow);
    });
    co_return HttpResponse("done");
  });

  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), "GET /slow HTTP/1.1\r\nHost: x\r\n\r\n");
  ASSERT_TRUE(WaitFor(started));
  ASSERT_TRUE(ShutdownWrite(client.fd()));
  std::this_thread::sleep_for(20ms);
  allow.store(true, std::memory_order_release);
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_TRUE(resp.ends_with("\r\n\r\ndone")) << resp;
}

// Requests pipelined behind a response closing the connection are not served.
TEST(HttpAsyncHandlers, RequestsPipelinedAfterConnectionCloseAreNotServed) {
  std::atomic_bool otherServed{false};
  ts.resetRouterAndGet().setPath(http::Method::GET, "/async", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    const int value = co_await req.deferWork([] { return 1; });
    co_return HttpResponse("async-" + std::to_string(value));
  });
  ts.router().setPath(http::Method::GET, "/other", [&otherServed](const HttpRequestView&) {
    otherServed.store(true);
    return HttpResponse("other");
  });

  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(),
                "GET /async HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\nGET /other HTTP/1.1\r\nHost: x\r\n\r\n");
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_TRUE(resp.ends_with("async-1")) << resp;
  EXPECT_FALSE(otherServed.load());
}

// The deferred work uses the server to post its completion: destroying the server waits for it.
TEST(HttpAsyncHandlers, ServerDestructionWaitsForDeferredWork) {
  std::atomic_bool started{false};
  std::atomic_bool workDone{false};
  {
    SingleHttpServer server{HttpServerConfig{}};
    server.router().setPath(http::Method::GET, "/slow", [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
      co_await req.deferWork([&] {
        started.store(true, std::memory_order_release);
        std::this_thread::sleep_for(200ms);
        workDone.store(true, std::memory_order_release);
      });
      co_return HttpResponse("done");
    });
    server.start();
    test::ClientConnection client(server.port());
    test::sendAll(client.fd(), "GET /slow HTTP/1.1\r\nHost: x\r\n\r\n");
    ASSERT_TRUE(WaitFor(started));
  }
  EXPECT_TRUE(workDone.load());
}

namespace {

struct NotDefaultConstructible {
  explicit NotDefaultConstructible(int val) : value(val) {}

  int value;
};

}  // namespace

TEST(HttpAsyncHandlers, DeferWorkSupportsVoidMoveOnlyAndNonDefaultConstructibleResults) {
  ts.resetRouterAndGet().setPath(http::Method::GET, "/types", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    std::atomic_int sideEffect{0};
    co_await req.deferWork([&sideEffect] { sideEffect.store(1); });
    const int fromMoveOnly = co_await req.deferWork([ptr = std::make_unique<int>(7)] { return *ptr; });
    const NotDefaultConstructible custom = co_await req.deferWork([] { return NotDefaultConstructible(4); });
    co_return HttpResponse(std::to_string(sideEffect.load() + fromMoveOnly + custom.value));
  });

  const std::string resp = test::simpleGet(ts.port(), "/types");
  EXPECT_TRUE(resp.ends_with("\r\n\r\n12")) << resp;
}

// Reading the body with readBody() made the request metrics throw (they called body()).
TEST(HttpAsyncHandlers, StreamedBodyIsCountedInRequestMetrics) {
  std::atomic<std::size_t> bytesIn{0};
  ts.server.setMetricsCallback([&bytesIn](const RequestMetrics& metrics) { bytesIn.store(metrics.bytesIn); });
  ts.resetRouterAndGet().setPath(http::Method::POST, "/stream", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    std::string collected;
    while (req.hasMoreBody()) {
      collected.append(co_await req.readBodyAsync(3));
    }
    co_return HttpResponse(collected);
  });

  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), PostHead("/stream", 11, "Connection: close\r\n") + "hello world");
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_TRUE(resp.ends_with("\r\n\r\nhello world")) << resp;
  EXPECT_EQ(bytesIn.load(), 11U);

  // Streaming handlers report the body size as well.
  bytesIn.store(0);
  ts.router().setPath(http::Method::POST, "/writer", [](const HttpRequestView&, HttpResponseWriter& writer) {
    writer.writeBody("written");
    writer.end();
  });
  test::ClientConnection writerClient(ts.port());
  test::sendAll(writerClient.fd(), PostHead("/writer", 5, "Connection: close\r\n") + "hello");
  const std::string writerResp = test::recvUntilClosed(writerClient.fd(), std::chrono::seconds{3});
  ts.server.setMetricsCallback({});
  EXPECT_TRUE(writerResp.contains("written")) << writerResp;
  EXPECT_EQ(bytesIn.load(), 5U);
}

#if defined(AERONET_ENABLE_HTTP2) && defined(AERONET_ENABLE_OPENSSL)

namespace {
test::TlsHttp2TestServer ts2;
}  // namespace

// The completion of the deferred work of a reset stream used to resume the coroutine of a later stream allocated at the
// same address (its frame was destroyed with the stream).
TEST(Http2AsyncHandlers, CompletionOfResetStreamDoesNotResumeAnotherStream) {
  ts2.server.resetRouterAndGet().setPath(
      http::Method::GET, "/value", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
        const auto delay = std::chrono::milliseconds(req.queryParamInt("delay").value_or(0));
        std::string value(req.queryParamValueOrEmpty("value"));
        std::string result = co_await req.deferWork([delay, value = std::move(value)] {
          std::this_thread::sleep_for(delay);
          return value;
        });
        co_return HttpResponse(result);
      });

  test::TlsHttp2Client client(ts2.port());
  ASSERT_TRUE(client.isConnected());
  const uint32_t first = client.sendAsyncRequest("GET", "/value?delay=200&value=first");
  std::this_thread::sleep_for(50ms);
  client.connection().sendRstStream(first, http2::ErrorCode::Cancel);
  // Flushes the RST_STREAM as well.
  const uint32_t second = client.sendAsyncRequest("GET", "/value?delay=600&value=second");
  const auto resp = client.waitAndGetResponse(second, 5000ms).value_or(test::TlsHttp2Client::Response{});
  EXPECT_EQ(resp.statusCode, 200);
  EXPECT_EQ(resp.body, "second");
}

TEST(Http2AsyncHandlers, CoroutineFrameOutlivesItsResetStreamUntilWorkCompletes) {
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  std::atomic_bool frameDestroyed{false};
  std::atomic_bool frameAliveAtWorkEnd{false};
  ts2.server.resetRouterAndGet().setPath(http::Method::GET, "/slow",
                                         [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
                                           DestructionFlag guard(frameDestroyed);
                                           co_await req.deferWork([&] {
                                             started.store(true, std::memory_order_release);
                                             WaitFor(allow);
                                             frameAliveAtWorkEnd.store(!frameDestroyed.load() && req.path() == "/slow");
                                           });
                                           co_return HttpResponse("done");
                                         });

  test::TlsHttp2Client client(ts2.port());
  ASSERT_TRUE(client.isConnected());
  const uint32_t streamId = client.sendAsyncRequest("GET", "/slow");
  ASSERT_TRUE(WaitFor(started));
  client.connection().sendRstStream(streamId, http2::ErrorCode::Cancel);
  // Flushes the RST_STREAM, and checks that the connection still works.
  const auto other = client.waitAndGetResponse(client.sendAsyncRequest("GET", "/unknown"), 5000ms)
                         .value_or(test::TlsHttp2Client::Response{});
  EXPECT_EQ(other.statusCode, 404);
  EXPECT_FALSE(frameDestroyed.load());

  allow.store(true, std::memory_order_release);
  EXPECT_TRUE(WaitFor(frameDestroyed));
  EXPECT_TRUE(frameAliveAtWorkEnd.load());
}

TEST(Http2AsyncHandlers, CoroutineFrameOutlivesItsClosedConnectionUntilWorkCompletes) {
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  std::atomic_bool frameDestroyed{false};
  std::atomic_bool frameAliveAtWorkEnd{false};
  ts2.server.resetRouterAndGet().setPath(http::Method::GET, "/slow",
                                         [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
                                           DestructionFlag guard(frameDestroyed);
                                           co_await req.deferWork([&] {
                                             started.store(true, std::memory_order_release);
                                             WaitFor(allow);
                                             frameAliveAtWorkEnd.store(!frameDestroyed.load() && req.path() == "/slow");
                                           });
                                           co_return HttpResponse("done");
                                         });

  {
    test::TlsHttp2Client client(ts2.port());
    ASSERT_TRUE(client.isConnected());
    client.sendAsyncRequest("GET", "/slow");
    ASSERT_TRUE(WaitFor(started));
  }  // closes the connection
  std::this_thread::sleep_for(50ms);
  EXPECT_FALSE(frameDestroyed.load());

  allow.store(true, std::memory_order_release);
  EXPECT_TRUE(WaitFor(frameDestroyed));
  EXPECT_TRUE(frameAliveAtWorkEnd.load());
}

// The HTTP/2 body is fully received before dispatch: it can be read with readBody() / readBodyAsync() as well.
TEST(Http2AsyncHandlers, BodyCanBeStreamed) {
  ts2.server.resetRouterAndGet().setPath(http::Method::POST, "/collect",
                                         [](HttpRequestView& req) -> RequestTask<HttpResponse> {
                                           std::string collected;
                                           while (req.hasMoreBody()) {
                                             collected.append(co_await req.readBodyAsync(4));
                                           }
                                           co_return HttpResponse(collected);
                                         });
  ts2.server.router().setPath(http::Method::POST, "/first-chunk",
                              [](HttpRequestView& req) -> RequestTask<HttpResponse> {
                                const std::string_view chunk = co_await req.readBodyAsync(5);
                                co_return HttpResponse(chunk);
                              });

  std::atomic<std::size_t> bytesIn{0};
  ts2.server.server.setMetricsCallback([&bytesIn](const RequestMetrics& metrics) { bytesIn.store(metrics.bytesIn); });

  test::TlsHttp2Client client(ts2.port());
  ASSERT_TRUE(client.isConnected());
  auto resp = client.post("/collect", "hello http2 body");
  EXPECT_EQ(resp.statusCode, 200);
  EXPECT_EQ(resp.body, "hello http2 body");
  EXPECT_EQ(bytesIn.load(), 16U);
  ts2.server.server.setMetricsCallback({});

  resp = client.post("/first-chunk", "hello http2 body");
  EXPECT_EQ(resp.statusCode, 200);
  EXPECT_EQ(resp.body, "hello");
}

// HTTP/2 requests were not attached to their connection: the accessors below (used by request metrics and access logs
// as well) dereferenced a null pointer.
TEST(Http2AsyncHandlers, RequestExposesItsConnection) {
  ts2.server.resetRouterAndGet().setPath(
      http::Method::GET, "/connection", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
        std::string result(req.clientAddress());
        result.append("|").append(req.tlsVersion()).append("|").append(req.alpnProtocol());
        co_return HttpResponse(result);
      });

  test::TlsHttp2Client client(ts2.port());
  ASSERT_TRUE(client.isConnected());
  const auto resp = client.get("/connection");
  EXPECT_EQ(resp.statusCode, 200);
  EXPECT_TRUE(resp.body.starts_with("127.0.0.1")) << resp.body;
  EXPECT_TRUE(resp.body.contains("|TLSv1.")) << resp.body;
  EXPECT_TRUE(resp.body.ends_with("|h2")) << resp.body;
}

// The router may be updated while an HTTP/2 async handler is suspended.
TEST(Http2AsyncHandlers, RouterUpdatesWhileHandlerIsSuspended) {
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  ts2.server.resetRouterAndGet()
      .setPath(http::Method::GET, "/items/{id}",
               [captured = std::string(64, 'c'), &started, &allow](HttpRequestView& req) -> RequestTask<HttpResponse> {
                 co_await req.deferWork([&] {
                   started.store(true, std::memory_order_release);
                   WaitFor(allow);
                 });
                 std::string result(req.pathParamValueOrEmpty("id"));
                 result.append(":").append(std::to_string(captured.size()));
                 co_return HttpResponse(result);
               })
      .after([](const HttpRequestView&, HttpResponse& resp) { resp.headerAddLine("x-route", "original"); });

  test::TlsHttp2Client client(ts2.port());
  ASSERT_TRUE(client.isConnected());
  const uint32_t streamId = client.sendAsyncRequest("GET", "/items/42");
  ASSERT_TRUE(WaitFor(started));

  ts2.server.resetRouterAndGet()
      .setPath(http::Method::GET, "/items/{id}",
               [](HttpRequestView&) -> RequestTask<HttpResponse> { co_return HttpResponse("replaced"); })
      .after([](const HttpRequestView&, HttpResponse& resp) { resp.headerAddLine("x-route", "replaced"); });
  allow.store(true, std::memory_order_release);

  const auto resp = client.waitAndGetResponse(streamId, 5000ms).value_or(test::TlsHttp2Client::Response{});
  EXPECT_EQ(resp.body, "42:64");
  EXPECT_EQ(resp.header("x-route"), "replaced");
}

#endif

}  // namespace aeronet
