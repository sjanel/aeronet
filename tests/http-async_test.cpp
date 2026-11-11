#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/cors-policy.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-helpers.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/request-metrics.hpp"
#include "aeronet/request-task.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/socket-ops.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/test_util.hpp"
#include "aeronet/zerocopy-mode.hpp"

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/raw-chars.hpp"
#endif

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

// Sets a flag when destroyed (after 'destructionTime'): tells when the coroutine frame holding it is destroyed.
class DestructionFlag {
 public:
  explicit DestructionFlag(std::atomic_bool& flag, std::chrono::milliseconds destructionTime = {}) noexcept
      : _flag(&flag), _destructionTime(destructionTime) {}

  DestructionFlag(const DestructionFlag&) = delete;
  DestructionFlag(DestructionFlag&&) noexcept = delete;
  DestructionFlag& operator=(const DestructionFlag&) = delete;
  DestructionFlag& operator=(DestructionFlag&&) noexcept = delete;

  ~DestructionFlag() {
    std::this_thread::sleep_for(_destructionTime);
    _flag->store(true, std::memory_order_release);
  }

 private:
  std::atomic_bool* _flag;
  std::chrono::milliseconds _destructionTime;
};

std::string PostHead(std::string_view target, std::size_t contentLength, std::string_view extraHeaders = {}) {
  std::string head("POST ");
  head.append(target).append(" HTTP/1.1\r\nHost: x\r\n").append(extraHeaders);
  head.append("Content-Length: ").append(std::to_string(contentLength)).append("\r\n\r\n");
  return head;
}

// Sends the head of 'request', then the rest of it once its handler raised 'dispatched', and returns the response (the
// request must close its connection).
std::string SendBodyAfterDispatch(uint16_t port, std::string_view request, std::atomic_bool& dispatched) {
  static constexpr std::string_view kEndOfHead = "\r\n\r\n";
  const std::size_t headSize = request.find(kEndOfHead) + kEndOfHead.size();
  dispatched.store(false, std::memory_order_release);
  test::ClientConnection client(port);
  test::sendAll(client.fd(), request.substr(0, headSize));
  EXPECT_TRUE(WaitFor(dispatched));
  test::sendAll(client.fd(), request.substr(headSize));
  return test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
}

constexpr auto kShortTimeout = 300ms;

HttpServerConfig ShortKeepAliveConfig() {
  HttpServerConfig cfg;
  cfg.withKeepAliveTimeout(kShortTimeout);
  return cfg;
}

HttpServerConfig ShortBodyReadConfig() {
  HttpServerConfig cfg;
  cfg.withBodyReadTimeout(kShortTimeout);
  return cfg;
}

// The tests below send within kShortTimeout of the restart of a timer, which a loaded CI runner can miss: its sleeps
// overshoot by up to a second on macOS. Such an attempt proves nothing: it is retried, a few times at most.
constexpr int kMaxSendInTimeAttempts = 3;

// Sends 'data' unless kShortTimeout elapsed since 'since' (the earliest time the timer may have restarted), and tells
// whether it was all sent in time. A late client is not sent to: the server may have closed its connection.
bool SendInTime(NativeHandle fd, std::string_view data, std::chrono::steady_clock::time_point since) {
  const auto deadline = since + kShortTimeout;
  if (std::chrono::steady_clock::now() >= deadline) {
    return false;
  }
  test::sendAll(fd, data);
  return std::chrono::steady_clock::now() < deadline;
}

void SkipAsNeverSentInTime() {
  GTEST_SKIP() << "the client never sent in time in " << kMaxSendInTimeAttempts << " attempts (loaded machine)";
}

// Uploads a body byte after byte for longer than kShortTimeout, each byte coming well within it, and expects its echo.
void ExpectUploadMakingProgressIsServed(HttpServerConfig cfg) {
  test::TestServer server(std::move(cfg));
  server.router().setPath(http::Method::POST, "/upload", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    co_return HttpResponse(co_await req.bodyAwaitable());
  });

  static constexpr std::string_view kBody = "abcdef";
  for (int attempt = 0; attempt < kMaxSendInTimeAttempts; ++attempt) {
    test::ClientConnection client(server.port());
    auto lastSendStart = std::chrono::steady_clock::now();
    test::sendAll(client.fd(), PostHead("/upload", kBody.size(), "Connection: close\r\n"));
    bool inTime = true;
    for (std::size_t pos = 0; inTime && pos < kBody.size(); ++pos) {
      std::this_thread::sleep_for(kShortTimeout / 3);
      const auto sendStart = std::chrono::steady_clock::now();
      inTime = SendInTime(client.fd(), kBody.substr(pos, 1), lastSendStart);
      lastSendStart = sendStart;
    }
    if (inTime) {
      const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
      EXPECT_TRUE(resp.ends_with(std::format("\r\n\r\n{}", kBody))) << resp;
      return;
    }
  }
  SkipAsNeverSentInTime();
}

// Deferred work is not idleness, and the client gets a whole window once the handler waits for its body: it may have
// been waiting for 100 Continue, only sent then.
void ExpectDeferredWorkIsNotIdleness(HttpServerConfig cfg) {
  // Before the server, which waits for the deferred work when destroyed.
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  std::atomic_bool workDone{false};
  // Atomic: written by the handler of the next attempt, without synchronization with the reads of the previous one.
  std::atomic<std::chrono::steady_clock::time_point> workEnd;
  test::TestServer server(std::move(cfg));
  server.router().setPath(http::Method::POST, "/upload", [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
    co_await req.deferWork([&] {
      started.store(true, std::memory_order_release);
      WaitFor(allow);
    });
    workEnd.store(std::chrono::steady_clock::now(), std::memory_order_relaxed);
    workDone.store(true, std::memory_order_release);
    co_return HttpResponse(co_await req.bodyAwaitable());
  });

  for (int attempt = 0; attempt < kMaxSendInTimeAttempts; ++attempt) {
    // The handler of the previous attempt no longer uses them.
    started.store(false, std::memory_order_relaxed);
    allow.store(false, std::memory_order_relaxed);
    workDone.store(false, std::memory_order_relaxed);
    test::ClientConnection client(server.port());
    test::sendAll(client.fd(), PostHead("/upload", 5, "Connection: close\r\n"));
    ASSERT_TRUE(WaitFor(started));
    std::this_thread::sleep_for(2 * kShortTimeout);
    allow.store(true, std::memory_order_release);
    ASSERT_TRUE(WaitFor(workDone));
    // Long enough for a sweep to close the connection if the window had ended during the work.
    std::this_thread::sleep_for(kShortTimeout / 3);
    // The window restarts once the handler waits for its body, after the end of the work.
    if (SendInTime(client.fd(), "hello", workEnd.load(std::memory_order_relaxed))) {
      const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
      EXPECT_TRUE(resp.ends_with("\r\n\r\nhello")) << resp;
      return;
    }
  }
  SkipAsNeverSentInTime();
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
  EXPECT_EQ(test::countOccurrences(resp, http::HTTP11SPSv), 1) << resp.substr(0, 512);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 401")) << resp.substr(0, 512);
  // The connection closes after it, as it announces (RFC 9110 §10.1.1).
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine(http::Connection, http::close))) << resp.substr(0, 512);
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
  EXPECT_EQ(test::countOccurrences(resp, http::HTTP11SPSv), 1) << resp.substr(0, 512);
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 403")) << resp.substr(0, 512);
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine(http::Connection, http::close))) << resp.substr(0, 512);
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
  EXPECT_EQ(test::countOccurrences(resp, http::HTTP11SPSv), 1) << resp.substr(0, 512);
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine(http::Connection, http::close))) << resp.substr(0, 512);
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

#ifdef AERONET_ENABLE_ZLIB
// The trailers of a compressed body decompressing to nothing are in the buffer of their connection, like those of any
// other body: they survive what other connections do while the handler is suspended (here a merged duplicate header
// writing in the temporary buffer shared by the connections).
TEST(HttpAsyncHandlers, TrailersOfBodyDecompressedToNothingSurviveOtherConnections) {
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/first", [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
    const std::string_view body = co_await req.bodyAwaitable();
    co_await req.deferWork([&] {
      started.store(true, std::memory_order_release);
      WaitFor(allow);
    });
    co_return HttpResponse(std::format("[{}|{}]", body, req.trailerValueOrEmpty("x-sig")));
  });
  ts.router().setPath(http::Method::GET, "/other", [](const HttpRequestView&) { return HttpResponse("other"); });

  static constexpr std::string_view kGzipOfNothing("\x1f\x8b\x08\0\0\0\0\0\0\x03\x03\0\0\0\0\0\0\0\0\0", 20);
  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), std::format("POST /first HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n"
                                         "Content-Encoding: gzip\r\nConnection: close\r\n\r\n"
                                         "{:x}\r\n{}\r\n0\r\nx-sig: secret-value\r\n\r\n",
                                         kGzipOfNothing.size(), kGzipOfNothing));
  ASSERT_TRUE(WaitFor(started));
  const std::string other = test::sendAndCollect(ts.port(),
                                                 "GET /other HTTP/1.1\r\nHost: h\r\nConnection: close\r\n"
                                                 "X-Dup: v1\r\nX-Dup: ZZZZZZZZZZZZZZZZ\r\n\r\n");
  EXPECT_TRUE(other.ends_with("other")) << other;
  allow.store(true, std::memory_order_release);
  const std::string resp = test::recvUntilClosed(client.fd());
  EXPECT_TRUE(resp.ends_with("[|secret-value]")) << resp.substr(0, 512);
}

namespace {

// A POST request whose body is 'payload' compressed with gzip 'nbStages' times.
std::string GzipPost(std::string_view target, std::string_view payload, int nbStages,
                     std::string_view extraHeaders = {}) {
  RawChars body = test::Compress(Encoding::gzip, payload);
  std::string encodings("gzip");
  for (int stage = 1; stage < nbStages; ++stage) {
    body = test::Compress(Encoding::gzip, body);
    encodings.append(", gzip");
  }
  std::string request =
      PostHead(target, body.size(), std::format("{}Content-Encoding: {}\r\n", extraHeaders, encodings));
  request.append(std::string_view(body));
  return request;
}

}  // namespace

// The body decompressed for an async handler and its updated Content-Length are at the start of the buffer shared by
// the connections: the connection takes it over when the handler suspends, so that they survive the compressed uploads
// of other connections, including the views taken before its first suspension (a fixed-length body received with its
// head), and later suspensions do not give it back. One stage ends in the temporary buffer swapped with the shared one,
// two stages end directly in the latter. A body decompressing to nothing still has its Content-Length there.
TEST(HttpAsyncHandlers, DecompressedBodySurvivesCompressedUploadsOfOtherConnections) {
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/hold", [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
    const std::string_view body = co_await req.bodyAwaitable();
    const std::string_view contentLength = req.headerValueOrEmpty(http::ContentLength);
    for (int suspension = 0; suspension < 2; ++suspension) {
      co_await req.deferWork([&] {
        started.store(true, std::memory_order_release);
        WaitFor(allow);
        allow.store(false, std::memory_order_release);
      });
    }
    co_return HttpResponse(std::format("[{}|{}]", contentLength, body));
  });
  ts.router().setPath(http::Method::POST, "/other", [](const HttpRequestView& req) {
    return HttpResponse(std::format("other:{}", req.body().size()));
  });

  for (const std::string& held : {std::string(9000, 'A'), std::string()}) {
    for (const int nbStages : {1, 2}) {
      started.store(false, std::memory_order_release);
      test::ClientConnection client(ts.port());
      test::sendAll(client.fd(), GzipPost("/hold", held, nbStages, "Connection: close\r\n"));
      for (int suspension = 0; suspension < 2; ++suspension) {
        ASSERT_TRUE(WaitFor(started));
        started.store(false, std::memory_order_release);
        // Two uploads: the decompression of each of them swaps the two buffers shared by the connections.
        for (const char letter : {'B', 'C'}) {
          const std::string other = test::sendAndCollect(
              ts.port(), GzipPost("/other", std::string(9800, letter), 1, "Connection: close\r\n"));
          EXPECT_TRUE(other.ends_with("other:9800")) << other.substr(0, 512);
        }
        allow.store(true, std::memory_order_release);
      }
      const std::string resp = test::recvUntilClosed(client.fd());
      EXPECT_TRUE(resp.ends_with(std::format("[{}|{}]", held.size(), held)))
          << held.size() << " bytes, " << nbStages << " stage(s): " << resp.substr(0, 512);
    }
  }
}

// Decompressing a body used to select the aggregated access for the handler: a hasMoreBody() loop read nothing, and
// readBodyAsync() threw. Whether the body is received with its head or after the dispatch of the handler.
TEST(HttpAsyncHandlers, CompressedBodyCanBeReadInChunks) {
  std::atomic_bool dispatched{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/loop",
                                 [&dispatched](HttpRequestView& req) -> RequestTask<HttpResponse> {
                                   dispatched.store(true, std::memory_order_release);
                                   std::string received;
                                   while (req.hasMoreBody()) {
                                     const std::string_view chunk = co_await req.readBodyAsync(64);
                                     received.append(chunk);
                                   }
                                   co_return HttpResponse(std::move(received));
                                 });
  ts.router().setPath(http::Method::POST, "/direct", [&dispatched](HttpRequestView& req) -> RequestTask<HttpResponse> {
    dispatched.store(true, std::memory_order_release);
    co_return HttpResponse(co_await req.readBodyAsync(5));
  });

  const std::string payload = std::string(150, 'a') + std::string(150, 'b');
  for (const auto& [target, expected] : {
           std::pair<std::string_view, std::string_view>{"/loop", payload},
           std::pair<std::string_view, std::string_view>{"/direct", "aaaaa"},
       }) {
    const std::string request = GzipPost(target, payload, 1, "Connection: close\r\n");
    const std::string atOnce = test::sendAndCollect(ts.port(), request);
    EXPECT_TRUE(atOnce.ends_with(std::format("\r\n\r\n{}", expected))) << target << ": " << atOnce.substr(0, 512);
    const std::string inTwoParts = SendBodyAfterDispatch(ts.port(), request, dispatched);
    EXPECT_TRUE(inTwoParts.ends_with(std::format("\r\n\r\n{}", expected)))
        << target << ": " << inTwoParts.substr(0, 512);
  }
}

// The requests pipelined on a connection take turns in its decompression buffer: each one reads its own body and its
// own Content-Length, whatever the size and the number of stages of the previous one.
TEST(HttpAsyncHandlers, PipelinedCompressedRequestsReadTheirOwnBodies) {
  ts.resetRouterAndGet().setPath(http::Method::POST, "/async", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    const std::string_view body = co_await req.bodyAwaitable();
    co_await req.deferWork([] {});
    co_return HttpResponse(std::format("[{}|{}]", req.headerValueOrEmpty(http::ContentLength), body));
  });
  ts.router().setPath(http::Method::POST, "/sync", [](const HttpRequestView& req) {
    return HttpResponse(std::format("[{}|{}]", req.headerValueOrEmpty(http::ContentLength), req.body()));
  });

  const std::string longBody(300, 'a');
  const std::string shortBody(20, 'b');
  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), GzipPost("/async", longBody, 1) + GzipPost("/async", shortBody, 2) +
                                 GzipPost("/sync", "sync", 1) + GzipPost("/async", "last", 2, "Connection: close\r\n"));
  const std::string resp = test::recvUntilClosed(client.fd());
  EXPECT_EQ(test::countOccurrences(resp, "HTTP/1.1 200"), 4) << resp.substr(0, 1024);
  EXPECT_TRUE(resp.contains(std::format("[300|{}]", longBody))) << resp.substr(0, 1024);
  EXPECT_TRUE(resp.contains(std::format("[20|{}]", shortBody))) << resp.substr(0, 1024);
  EXPECT_TRUE(resp.contains("[4|sync]")) << resp.substr(0, 1024);
  EXPECT_TRUE(resp.ends_with("[4|last]")) << resp.substr(0, 1024);
}
#endif

// An async handler request pipelined behind a served one, from the same input: its views (here into its body, taken
// before suspending) survive the bytes received while it is suspended, and the requests around it are served once.
TEST(HttpAsyncHandlers, PipelinedBodyViewsSurviveBytesReceivedWhileSuspended) {
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  ts.resetRouterAndGet().setPath(
      http::Method::POST, "/async/{id}", [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
        const std::string_view body = req.body();
        co_await req.deferWork([&] {
          started.store(true, std::memory_order_release);
          WaitFor(allow);
        });
        co_return HttpResponse(std::format("[{}|{}|{}|{}]", req.pathParamValueOrEmpty("id"),
                                           req.queryParamValueOrEmpty("q"), req.headerValueOrEmpty("x-h"), body));
      });
  ts.router().setPath(http::Method::POST, "/sync", [](const HttpRequestView& req) {
    return HttpResponse(std::format("[sync:{}]", req.body().size()));
  });

  static constexpr std::size_t kPipelinedBody = std::size_t{1} << 20U;
  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), PostHead("/sync", 3) + "abc" + PostHead("/async/7?q=v", 5, "X-H: hv\r\n") + "hello");
  ASSERT_TRUE(WaitFor(started));
  // The server may not read these bytes before the handler completes: send them from another thread.
  std::jthread sender([fd = client.fd()] {
    test::sendAll(fd, PostHead("/sync", kPipelinedBody, "Connection: close\r\n") + std::string(kPipelinedBody, 'y'),
                  10000ms);
  });
  std::this_thread::sleep_for(100ms);
  allow.store(true, std::memory_order_release);
  const std::string resp = test::recvUntilClosed(client.fd());
  sender.join();
  EXPECT_EQ(test::countOccurrences(resp, "HTTP/1.1 200"), 3) << resp.substr(0, 1024);
  EXPECT_TRUE(resp.contains("[sync:3]")) << resp.substr(0, 1024);
  EXPECT_TRUE(resp.contains("[7|v|hv|hello]")) << resp.substr(0, 1024);
  EXPECT_TRUE(resp.ends_with("[sync:1048576]")) << resp.substr(0, 1024);
}

// An async handler request pipelined behind a served one waits for its body, received after its head.
TEST(HttpAsyncHandlers, PipelinedRequestWaitingForItsBody) {
  ts.resetRouterAndGet().setPath(
      http::Method::POST, "/async/{id}", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
        const std::string_view body = co_await req.bodyAwaitable();
        co_return HttpResponse(std::format("[{}|{}|{}|{}]", req.pathParamValueOrEmpty("id"),
                                           req.queryParamValueOrEmpty("q"), req.headerValueOrEmpty("x-h"), body));
      });
  ts.router().setPath(http::Method::GET, "/sync", [](const HttpRequestView&) { return HttpResponse("[sync]"); });

  for (const bool chunked : {false, true}) {
    SCOPED_TRACE(chunked);
    test::ClientConnection client(ts.port());
    ASSERT_TRUE(SetTcpNoDelay(client.fd()));  // a part is not delayed until the previous one is acknowledged
    const std::string asyncHead =
        chunked ? "POST /async/7?q=v HTTP/1.1\r\nHost: x\r\nX-H: hv\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel"
                : PostHead("/async/7?q=v", 5, "X-H: hv\r\n") + "hel";
    test::sendAll(client.fd(), "GET /sync HTTP/1.1\r\nHost: x\r\n\r\n" + asyncHead);
    std::this_thread::sleep_for(20ms);
    test::sendAll(client.fd(), std::string(chunked ? "lo\r\n0\r\n\r\n" : "lo") +
                                   "GET /sync HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
    EXPECT_EQ(test::countOccurrences(resp, "HTTP/1.1 200"), 3) << resp;
    const auto asyncPos = resp.find("[7|v|hv|hello]");
    ASSERT_NE(asyncPos, std::string::npos) << resp;
    EXPECT_LT(resp.find("[sync]"), asyncPos) << resp;
    EXPECT_TRUE(resp.ends_with("[sync]")) << resp;
  }
}

// The body of a chunked request, whose async handler waits for it, is decoded by pieces as it is received.
TEST(HttpAsyncHandlers, ChunkedBodyReceivedInSeveralReadsAfterDispatch) {
  ts.resetRouterAndGet().setPath(http::Method::POST, "/chunked", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    const std::string_view body = co_await req.bodyAwaitable();
    co_return HttpResponse(std::format("{}|{}", body, req.trailerValueOrEmpty("x-end")));
  });

  test::ClientConnection client(ts.port());
  ASSERT_TRUE(SetTcpNoDelay(client.fd()));  // a part is not delayed until the previous one is acknowledged
  test::sendAll(client.fd(),
                "POST /chunked HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
  // The reads end in a size line, a chunk data, the CRLF after it, the last chunk and the trailers.
  for (const std::string_view part :
       {"3\r\nfoo\r\n", "6;e=1\r\nbarb", "az", "\r\n", "0\r\nX-En", "d: done\r\n", "\r\n"}) {
    std::this_thread::sleep_for(15ms);
    test::sendAll(client.fd(), part);
  }
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
  EXPECT_TRUE(resp.ends_with("foobarbaz|done")) << resp;
}

// Requests pipelined behind an async handler were only served when new bytes arrived.
// Handlers completing right away are consumed at once with the requests pipelined around them, including one that
// suspends: its completion then erases what was served before it.
TEST(HttpAsyncHandlers, PipelinedRequestsCompletingRightAwayAroundADeferredOne) {
  ts.resetRouterAndGet().setPath(http::Method::POST, "/now", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    co_return HttpResponse(co_await req.bodyAwaitable());
  });
  ts.router().setPath(http::Method::POST, "/later", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    const std::string_view body = co_await req.bodyAwaitable();
    co_await req.deferWork([] {});
    co_return HttpResponse(body);
  });

  const std::string requests = PostHead("/now", 2) + "aa" + PostHead("/now", 2) + "bb" + PostHead("/later", 2) + "cc" +
                               PostHead("/now", 2, "Connection: close\r\n") + "dd";
  const std::string resp = test::sendAndCollect(ts.port(), requests);
  EXPECT_EQ(test::countOccurrences(resp, http::HTTP11SPSv), 4) << resp;
  std::size_t pos = 0;
  for (const std::string_view body : {"\r\n\r\naa", "\r\n\r\nbb", "\r\n\r\ncc", "\r\n\r\ndd"}) {
    pos = resp.find(body, pos);
    ASSERT_NE(pos, std::string::npos) << body << " in " << resp;
  }
}

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

// What is kept while a request is served (its expectations answered, the 100 Continue its async handler owes) is
// forgotten once it ends: on a keep-alive connection, each request waiting for 100 Continue gets it, whether its
// handler is async or not.
TEST(HttpAsyncHandlers, ExpectContinueIsSentForEachRequestOfAKeepAliveConnection) {
  ts.resetRouterAndGet().setPath(http::Method::POST, "/sync", [](const HttpRequestView& req) {
    return HttpResponse(std::format("sync:{}", req.body()));
  });
  ts.router().setPath(http::Method::POST, "/async", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    const std::string_view body = co_await req.bodyAwaitable();
    co_return HttpResponse(std::format("async:{}", body));
  });

  test::ClientConnection client(ts.port());
  for (std::string_view target : {"/async", "/async", "/sync", "/async"}) {
    SCOPED_TRACE(target);
    test::sendAll(client.fd(), PostHead(target, 5, "Expect: 100-continue\r\n"));
    ASSERT_EQ(test::RecvHead(client.fd()), http::HTTP11_100_CONTINUE);
    test::sendAll(client.fd(), "hel");
    std::this_thread::sleep_for(20ms);
    test::sendAll(client.fd(), "lo");
    const std::string resp = test::recvWithTimeout(client.fd(), 2s);
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 200 ")) << resp;
    EXPECT_TRUE(resp.ends_with(std::format("\r\n\r\n{}:hello", target.substr(1)))) << resp;
  }
}

// 100 Continue is sent once the async handler waits for the body (here after deferred work), not when it is called: a
// handler answering from the head alone, or a request middleware, is answered without it (RFC 9110 §10.1.1). The
// connection then closes, as the response announces: the client may send the body anyway.
TEST(HttpAsyncHandlers, ExpectContinueIsSentWhenTheHandlerWaitsForTheBody) {
  std::atomic<int> handlerCalls{0};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/deferred",
                                 [](HttpRequestView& req) -> RequestTask<HttpResponse> {
                                   const int value = co_await req.deferWork([] { return 7; });
                                   const std::string_view body = co_await req.bodyAwaitable();
                                   co_return HttpResponse(std::format("{}:{}", value, body));
                                 });
  ts.router().setPath(http::Method::POST, "/reject", [](HttpRequestView&) -> RequestTask<HttpResponse> {
    co_return HttpResponse(http::StatusCodeForbidden, "rejected");
  });
  ts.router().setPath(http::Method::POST, "/deferred-reject", [](HttpRequestView& req) -> RequestTask<HttpResponse> {
    const bool allowed = co_await req.deferWork([] { return false; });
    co_return HttpResponse(allowed ? http::StatusCodeOK : http::StatusCodeForbidden, "rejected later");
  });
  ts.router()
      .setPath(http::Method::POST, "/guarded",
               [&handlerCalls](HttpRequestView&) -> RequestTask<HttpResponse> {
                 ++handlerCalls;
                 co_return HttpResponse("handled");
               })
      .before([](HttpRequestView&) {
        return MiddlewareResult::ShortCircuit(HttpResponse(http::StatusCodeUnauthorized, "denied"));
      });

  {
    test::ClientConnection client(ts.port());
    const std::string resp = test::SendExpectingContinue(
        client.fd(), PostHead("/deferred", 5, "Expect: 100-continue\r\nConnection: close\r\n"), "hello");
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 ")) << resp;
    EXPECT_TRUE(resp.ends_with("7:hello")) << resp;
  }
  {  // A body received along with its head does not need 100 Continue.
    const std::string resp = test::sendAndCollect(
        ts.port(), PostHead("/deferred", 5, "Expect: 100-continue\r\nConnection: close\r\n") + "hello");
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 200 ")) << resp;
    EXPECT_TRUE(resp.ends_with("7:hello")) << resp;
  }
  for (std::string_view target : {"/reject", "/deferred-reject", "/guarded"}) {
    SCOPED_TRACE(target);
    test::ClientConnection client(ts.port());
    const std::string resp =
        test::SendExpectingContinue(client.fd(), PostHead(target, 5, "Expect: 100-continue\r\n"), "hello");
    EXPECT_FALSE(resp.contains("100 Continue")) << resp;
    EXPECT_EQ(test::countOccurrences(resp, http::HTTP11SPSv), 1) << resp;
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 401 ") || resp.starts_with("HTTP/1.1 403 ")) << resp;
    EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine(http::Connection, http::close))) << resp;
  }
  EXPECT_EQ(handlerCalls.load(), 0);
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

// A request timing out while its deferred work runs is abandoned, even if its client keeps its socket open until the
// work ends: its coroutine is not resumed, and it no longer answers on (nor erases the input of) a closing connection.
// Its body may be complete or not.
TEST(HttpAsyncHandlers, TimedOutHandlerIsAbandonedWhileItsClientStaysConnected) {
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  std::atomic_bool resumed{false};
  std::atomic_bool frameDestroyed{false};
  ts.resetRouterAndGet()
      .setPath(http::Method::GET | http::Method::POST, "/slow",
               [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
                 DestructionFlag guard(frameDestroyed);
                 co_await req.deferWork([&] {
                   started.store(true, std::memory_order_release);
                   WaitFor(allow);
                 });
                 resumed.store(true, std::memory_order_release);
                 co_return HttpResponse("done");
               })
      .timeout(50ms);

  for (const std::string& request :
       {std::string("GET /slow HTTP/1.1\r\nHost: x\r\n\r\n"), PostHead("/slow", 10) + "abc"}) {
    started.store(false, std::memory_order_release);
    allow.store(false, std::memory_order_release);
    frameDestroyed.store(false, std::memory_order_release);

    test::ClientConnection client(ts.port());
    test::sendAll(client.fd(), request);
    ASSERT_TRUE(WaitFor(started));
    const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 408")) << resp;
    EXPECT_FALSE(frameDestroyed.load());

    allow.store(true, std::memory_order_release);
    EXPECT_TRUE(WaitFor(frameDestroyed));
    EXPECT_FALSE(resumed.load());
    EXPECT_TRUE(test::simpleGet(ts.port(), "/unknown").starts_with("HTTP/1.1 404"));
  }
}

// The deferred work of a request that timed out can still read its body: the input buffer holding it is not reused for
// the input of other connections.
TEST(HttpAsyncHandlers, BodyOfTimedOutHandlerStaysValidForItsDeferredWork) {
  static constexpr std::size_t kBodySize = 4096;
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  std::atomic_bool bodyIntact{false};
  std::atomic_bool frameDestroyed{false};
  ts.resetRouterAndGet()
      .setPath(http::Method::POST, "/slow",
               [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
                 DestructionFlag guard(frameDestroyed);
                 const std::string_view body = co_await req.bodyAwaitable();
                 co_await req.deferWork([&started, &allow, &bodyIntact, body] {
                   started.store(true, std::memory_order_release);
                   WaitFor(allow);
                   bodyIntact.store(body == std::string(kBodySize, 'A'));
                 });
                 co_return HttpResponse("done");
               })
      .timeout(50ms);
  ts.router().setPath(http::Method::POST, "/other", [](const HttpRequestView& req) {
    return HttpResponse(std::format("other:{}", req.body().size()));
  });

  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), PostHead("/slow", kBodySize) + std::string(kBodySize, 'A'));
  ASSERT_TRUE(WaitFor(started));
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 408")) << resp;

  // The input of these connections is read into the input buffer of the event loop.
  for (const char letter : {'B', 'C'}) {
    const std::string other = test::sendAndCollect(
        ts.port(), PostHead("/other", kBodySize, "Connection: close\r\n") + std::string(kBodySize, letter));
    EXPECT_TRUE(other.ends_with(std::format("other:{}", kBodySize))) << other.substr(0, 512);
  }

  allow.store(true, std::memory_order_release);
  EXPECT_TRUE(WaitFor(frameDestroyed));
  EXPECT_TRUE(bodyIntact.load());
}

#ifdef AERONET_ENABLE_ZLIB
// Likewise for a decompressed body: its connection took over the shared buffer holding it when the handler suspended,
// and keeps it until the deferred work completes, even once closed.
TEST(HttpAsyncHandlers, DecompressedBodyOfTimedOutHandlerStaysValidForItsDeferredWork) {
  static constexpr std::size_t kBodySize = 9000;
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  std::atomic_bool bodyRead{false};
  std::atomic_bool bodyIntact{false};
  ts.resetRouterAndGet()
      .setPath(http::Method::POST, "/slow",
               [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
                 const std::string_view body = co_await req.bodyAwaitable();
                 co_await req.deferWork([&started, &allow, &bodyRead, &bodyIntact, body] {
                   started.store(true, std::memory_order_release);
                   WaitFor(allow);
                   bodyIntact.store(body == std::string(kBodySize, 'A'));
                   bodyRead.store(true, std::memory_order_release);
                 });
                 co_return HttpResponse("done");
               })
      .timeout(50ms);
  ts.router().setPath(http::Method::POST, "/other", [](const HttpRequestView& req) {
    return HttpResponse(std::format("other:{}", req.body().size()));
  });

  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), GzipPost("/slow", std::string(kBodySize, 'A'), 1));
  ASSERT_TRUE(WaitFor(started));
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 408")) << resp;

  for (const char letter : {'B', 'C'}) {
    const std::string other =
        test::sendAndCollect(ts.port(), GzipPost("/other", std::string(9800, letter), 1, "Connection: close\r\n"));
    EXPECT_TRUE(other.ends_with("other:9800")) << other.substr(0, 512);
  }

  allow.store(true, std::memory_order_release);
  ASSERT_TRUE(WaitFor(bodyRead));
  EXPECT_TRUE(bodyIntact.load());
}
#endif

// A request timing out while its handler waits for the rest of its body has nothing left running: its coroutine is
// released right away, and its connection can linger, unread bytes of the body being likely.
TEST(HttpAsyncHandlers, TimeoutWhileWaitingForBodyReleasesTheCoroutine) {
  std::atomic_bool frameDestroyed{false};
  ts.resetRouterAndGet()
      .setPath(http::Method::POST, "/upload",
               [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
                 DestructionFlag guard(frameDestroyed);
                 const std::string_view body = co_await req.bodyAwaitable();
                 co_return HttpResponse(body);
               })
      .timeout(50ms);

  test::ClientConnection client(ts.port());
  test::sendAll(client.fd(), PostHead("/upload", 10) + "abc");
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 408")) << resp;
  // Well before the end of the lingering close (5 s), the client keeping its socket open.
  EXPECT_TRUE(WaitFor(frameDestroyed, 2000ms));
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

namespace {

// What follows the head of a body made of 'data', sent with a Content-Length or chunked with an optional trailer line.
struct AsyncUpload {
  std::string head;
  std::string body;
};

AsyncUpload FixedLengthGzipUpload(std::string_view data) {
  return {PostHead("/upload", data.size(), "Content-Encoding: gzip\r\nConnection: close\r\n"), std::string(data)};
}

AsyncUpload ChunkedGzipUpload(std::string_view data, std::string_view trailerLine) {
  return {
      "POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nContent-Encoding: gzip\r\nConnection: "
      "close\r\n\r\n",
      std::format("{:x}\r\n{}\r\n0\r\n{}\r\n", data.size(), data, trailerLine),
  };
}

}  // namespace

// The decompression of a body updates the headers of its request: it waits for the end of the deferred work, which may
// read them.
TEST(HttpAsyncHandlers, DeferredWorkReadsHeadersUnchangedByTheArrivalOfTheBody) {
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  std::atomic_bool headerStable{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/upload", [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
    co_await req.deferWork([&] {
      const std::string before(req.headerValueOrEmpty(http::ContentEncoding));
      started.store(true, std::memory_order_release);
      WaitFor(allow);
      headerStable.store(before == "gzip" && req.headerValueOrEmpty(http::ContentEncoding) == "gzip",
                         std::memory_order_release);
    });
    const std::string_view body = co_await req.bodyAwaitable();
    co_return HttpResponse(std::format("[{}|{}|{}]", req.headerValueOrEmpty(http::ContentEncoding), body,
                                       req.trailerValueOrEmpty("x-sig")));
  });

  const std::string payload(200, 'p');
  const RawChars compressed = test::Compress(Encoding::gzip, payload);
  const std::string_view compressedView(compressed);
  const auto expectStableHeaders = [&](const AsyncUpload& upload, std::string_view trailer) {
    started.store(false, std::memory_order_release);
    allow.store(false, std::memory_order_release);
    headerStable.store(false, std::memory_order_release);
    test::ClientConnection client(ts.port());
    test::sendAll(client.fd(), upload.head);
    ASSERT_TRUE(WaitFor(started));
    test::sendAll(client.fd(), upload.body);
    // Long enough for the server to receive the whole body while the work runs.
    std::this_thread::sleep_for(50ms);
    allow.store(true, std::memory_order_release);
    const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
    EXPECT_TRUE(resp.ends_with(std::format("[|{}|{}]", payload, trailer))) << resp.substr(0, 512);
    EXPECT_TRUE(headerStable.load(std::memory_order_acquire));
  };

  expectStableHeaders(FixedLengthGzipUpload(compressedView), "");
  expectStableHeaders(ChunkedGzipUpload(compressedView, ""), "");
  expectStableHeaders(ChunkedGzipUpload(compressedView, "x-sig: value\r\n"), "value");
}

// A body received during the deferred work is decompressed once it ends: a failure then answers instead of resuming
// the handler, and releases its coroutine.
TEST(HttpAsyncHandlers, UndecodableBodyReceivedDuringDeferredWorkIsRejectedOnceItEnds) {
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  std::atomic_bool resumed{false};
  std::atomic_bool frameDestroyed{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/upload", [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
    DestructionFlag guard(frameDestroyed);
    co_await req.deferWork([&] {
      started.store(true, std::memory_order_release);
      WaitFor(allow);
    });
    resumed.store(true, std::memory_order_release);
    co_return HttpResponse(co_await req.bodyAwaitable());
  });

  for (const AsyncUpload& upload : {FixedLengthGzipUpload("notgzipped"), ChunkedGzipUpload("notgzipped", "")}) {
    started.store(false, std::memory_order_release);
    allow.store(false, std::memory_order_release);
    frameDestroyed.store(false, std::memory_order_release);
    test::ClientConnection client(ts.port());
    test::sendAll(client.fd(), upload.head);
    ASSERT_TRUE(WaitFor(started));
    test::sendAll(client.fd(), upload.body);
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(frameDestroyed.load(std::memory_order_acquire));
    allow.store(true, std::memory_order_release);
    const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 4")) << resp;
    EXPECT_TRUE(WaitFor(frameDestroyed));
  }
  EXPECT_FALSE(resumed.load(std::memory_order_acquire));
}

// Nothing to decompress for a compressed chunked body without any chunk.
TEST(HttpAsyncHandlers, EmptyCompressedChunkedBodyReceivedAfterDispatch) {
  std::atomic_bool dispatched{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/upload",
                                 [&dispatched](HttpRequestView& req) -> RequestTask<HttpResponse> {
                                   dispatched.store(true, std::memory_order_release);
                                   const std::string_view body = co_await req.bodyAwaitable();
                                   co_return HttpResponse(std::format("[{}]", body));
                                 });

  const std::string request = ChunkedGzipUpload("", "").head + "0\r\n\r\n";
  const std::string resp = SendBodyAfterDispatch(ts.port(), request, dispatched);
  EXPECT_TRUE(resp.ends_with("\r\n\r\n[]")) << resp;
}

// Once its handler answered, a body is only drained: it is not decompressed, so that a corrupted one changes nothing.
TEST(HttpAsyncHandlers, BodyOfAnAnsweredRequestIsNotDecompressed) {
  std::atomic_bool dispatched{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/upload",
                                 [&dispatched](HttpRequestView&) -> RequestTask<HttpResponse> {
                                   dispatched.store(true, std::memory_order_release);
                                   co_return HttpResponse("early");
                                 });

  for (const AsyncUpload& upload : {FixedLengthGzipUpload("notgzipped"), ChunkedGzipUpload("notgzipped", "")}) {
    const std::string resp = SendBodyAfterDispatch(ts.port(), upload.head + upload.body, dispatched);
    EXPECT_TRUE(resp.starts_with("HTTP/1.1 200")) << resp;
    EXPECT_TRUE(resp.ends_with("\r\n\r\nearly")) << resp;
  }
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

// hasMoreBody() is true while the body is being received, but only readBodyAsync() can wait for it: readBody()
// dereferenced a null pointer.
TEST(HttpAsyncHandlers, ReadBodyBeforeTheBodyIsReceivedThrows) {
  std::atomic_bool dispatched{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/uncaught",
                                 [&dispatched](HttpRequestView& req) -> RequestTask<HttpResponse> {
                                   dispatched.store(true, std::memory_order_release);
                                   co_return HttpResponse(req.readBody());
                                 });
  // A failed readBody() does not commit the request to the streaming style: the handler can still wait for its body.
  ts.router().setPath(http::Method::POST, "/caught", [&dispatched](HttpRequestView& req) -> RequestTask<HttpResponse> {
    dispatched.store(true, std::memory_order_release);
    const bool moreBodyAnnounced = req.hasMoreBody();
    bool threw = false;
    try {
      (void)req.readBody();
    } catch (const std::logic_error&) {
      threw = true;
    }
    const std::string_view body = co_await req.bodyAwaitable();
    co_return HttpResponse(moreBodyAnnounced && threw ? body : std::string_view("unexpected"));
  });

  const auto postInTwoParts = [&dispatched](std::string_view target) {
    return SendBodyAfterDispatch(ts.port(), PostHead(target, 5, "Connection: close\r\n") + "hello", dispatched);
  };

  const std::string uncaught = postInTwoParts("/uncaught");
  EXPECT_TRUE(uncaught.starts_with("HTTP/1.1 500")) << uncaught;
  const std::string caught = postInTwoParts("/caught");
  EXPECT_TRUE(caught.ends_with("\r\n\r\nhello")) << caught;
}

// body() is empty until the body is received, but must not hide it: it used to make the body awaitables complete right
// away, with an empty body.
TEST(HttpAsyncHandlers, BodyCalledBeforeTheBodyIsReceivedDoesNotLoseIt) {
  std::atomic_bool dispatched{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/aggregated",
                                 [&dispatched](HttpRequestView& req) -> RequestTask<HttpResponse> {
                                   const bool nothingYet = req.body().empty() && req.hasMoreBody();
                                   dispatched.store(true, std::memory_order_release);
                                   const std::string_view body = co_await req.bodyAwaitable();
                                   co_return HttpResponse(nothingYet ? body : std::string_view("unexpected"));
                                 });
  ts.router().setPath(http::Method::POST, "/streamed",
                      [&dispatched](HttpRequestView& req) -> RequestTask<HttpResponse> {
                        (void)req.body();
                        dispatched.store(true, std::memory_order_release);
                        co_return HttpResponse(co_await req.readBodyAsync());
                      });
  ts.router()
      .setPath(http::Method::POST, "/middleware",
               [&dispatched](HttpRequestView& req) -> RequestTask<HttpResponse> {
                 dispatched.store(true, std::memory_order_release);
                 co_return HttpResponse(co_await req.bodyAwaitable());
               })
      .before([](HttpRequestView& req) {
        (void)req.body();
        return MiddlewareResult::Continue();
      });

  for (const std::string_view target : {"/aggregated", "/streamed", "/middleware"}) {
    const std::string resp =
        SendBodyAfterDispatch(ts.port(), PostHead(target, 5, "Connection: close\r\n") + "hello", dispatched);
    EXPECT_TRUE(resp.ends_with("\r\n\r\nhello")) << target << ": " << resp;
  }
}

// The event loop leaves the request alone while deferred work runs: a body received meanwhile only shows up once the
// work ends, right before the coroutine resumes.
TEST(HttpAsyncHandlers, RequestStaysUnchangedDuringDeferredWorkWhileItsBodyArrives) {
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  std::atomic_bool stable{false};
  ts.resetRouterAndGet().setPath(http::Method::POST, "/upload", [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
    co_await req.deferWork([&] {
      const auto bodyNotReadable = [&req] {
        return req.hasMoreBody() && !req.isBodyReady() && req.body().empty() && req.trailers().empty();
      };
      const bool notReadableBefore = bodyNotReadable();
      started.store(true, std::memory_order_release);
      WaitFor(allow);
      stable.store(notReadableBefore && bodyNotReadable(), std::memory_order_release);
    });
    const bool moreBody = req.hasMoreBody();
    const std::string_view body = co_await req.bodyAwaitable();
    co_return HttpResponse(std::format("[{}|{}|{}]", moreBody, body, req.trailerValueOrEmpty("x-sig")));
  });

  const auto expectBodyAfterTheWork = [&](std::string_view head, std::string_view body, std::string_view expected) {
    started.store(false, std::memory_order_release);
    allow.store(false, std::memory_order_release);
    stable.store(false, std::memory_order_release);
    test::ClientConnection client(ts.port());
    test::sendAll(client.fd(), head);
    ASSERT_TRUE(WaitFor(started));
    test::sendAll(client.fd(), body);
    // Long enough for the server to receive the whole body while the work runs.
    std::this_thread::sleep_for(50ms);
    allow.store(true, std::memory_order_release);
    const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
    EXPECT_TRUE(resp.ends_with(expected)) << resp;
    EXPECT_TRUE(stable.load(std::memory_order_acquire));
  };

  expectBodyAfterTheWork(PostHead("/upload", 5, "Connection: close\r\n"), "hello", "[true|hello|]");
  expectBodyAfterTheWork("POST /upload HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n",
                         "5\r\nhello\r\n0\r\nx-sig: value\r\n\r\n", "[true|hello|value]");
}

// The captures of the work are destroyed before its completion is posted: they may refer to objects that the end of the
// handler (or of the server) destroys.
TEST(HttpAsyncHandlers, WorkCapturesAreDestroyedBeforeTheHandlerResumes) {
  std::atomic_bool captureDestroyed{false};
  std::atomic_bool destroyedAtResume{false};
  ts.resetRouterAndGet().setPath(http::Method::GET, "/work", [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
    co_await req.deferWork([guard = std::make_unique<DestructionFlag>(captureDestroyed, 50ms)] {});
    destroyedAtResume.store(captureDestroyed.load(std::memory_order_acquire), std::memory_order_release);
    co_return HttpResponse("done");
  });

  const std::string resp =
      test::sendAndCollect(ts.port(), "GET /work HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  EXPECT_TRUE(resp.ends_with("\r\n\r\ndone")) << resp;
  EXPECT_TRUE(destroyedAtResume.load(std::memory_order_acquire));
}

// An answer before the body goes through the pipeline like any response: it is not an error of the parser. Its metrics
// count no body bytes, none being read (its head only announces their number).
TEST(HttpAsyncHandlers, HandlerThrowingWhenCalledBeforeItsBodyGoesThroughThePipeline) {
  std::atomic<int> nbParserErrors{0};
  std::atomic<int> metricsStatus{0};
  std::atomic<std::size_t> metricsBytesIn{1};
  test::TestServer server{HttpServerConfig{}};
  server.server.setParserErrorCallback([&nbParserErrors](http::StatusCode) { nbParserErrors.fetch_add(1); });
  server.server.setMetricsCallback([&metricsStatus, &metricsBytesIn](const RequestMetrics& metrics) {
    metricsStatus.store(metrics.status);
    metricsBytesIn.store(metrics.bytesIn);
  });
  server.router().addResponseMiddleware(
      [](const HttpRequestView&, HttpResponse& response) { response.headerAddLine("x-post", "1"); });
  server.router().setPath(http::Method::POST, "/throw",
                          [](HttpRequestView&) -> RequestTask<HttpResponse> { throw std::runtime_error("boom"); });

  test::ClientConnection client(server.port());
  test::sendAll(client.fd(), PostHead("/throw", 10) + "abc");
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_TRUE(resp.starts_with("HTTP/1.1 500")) << resp;
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine("x-post", "1"))) << resp;
  EXPECT_TRUE(resp.contains(MakeHttp1HeaderLine(http::Connection, http::close))) << resp;
  EXPECT_EQ(nbParserErrors.load(), 0);
  EXPECT_EQ(metricsStatus.load(), 500);
  EXPECT_EQ(metricsBytesIn.load(), 0U);
}

// keepAliveTimeout bounds a stalled upload as on a synchronous route, whether the handler awaits the body or already
// completed (it is then answered once its body is received).
TEST(HttpAsyncHandlers, StalledUploadIsClosedByTheKeepAliveTimeout) {
  std::atomic_bool frameDestroyed{false};
  test::TestServer server(ShortKeepAliveConfig());
  server.router().setPath(http::Method::POST, "/awaits",
                          [&frameDestroyed](HttpRequestView& req) -> RequestTask<HttpResponse> {
                            DestructionFlag guard(frameDestroyed);
                            co_return HttpResponse(co_await req.bodyAwaitable());
                          });
  server.router().setPath(http::Method::POST, "/completes",
                          [](HttpRequestView&) -> RequestTask<HttpResponse> { co_return HttpResponse("early"); });

  for (const std::string_view target : {"/awaits", "/completes"}) {
    test::ClientConnection client(server.port());
    test::sendAll(client.fd(), PostHead(target, 10) + "abc");
    EXPECT_TRUE(test::WaitForPeerClose(client.fd(), 5000ms)) << target;
  }
  EXPECT_TRUE(WaitFor(frameDestroyed, 2000ms));
}

// Only a client that stops sending is idle.
TEST(HttpAsyncHandlers, UploadMakingProgressIsNotClosedByTheKeepAliveTimeout) {
  ExpectUploadMakingProgressIsServed(ShortKeepAliveConfig());
}

// bodyReadTimeout resets whenever body bytes are received, as for a synchronous route.
TEST(HttpAsyncHandlers, UploadMakingProgressIsNotClosedByTheBodyReadTimeout) {
  ExpectUploadMakingProgressIsServed(ShortBodyReadConfig());
}

TEST(HttpAsyncHandlers, KeepAliveTimeoutDoesNotCountTheDeferredWork) {
  ExpectDeferredWorkIsNotIdleness(ShortKeepAliveConfig());
}

TEST(HttpAsyncHandlers, BodyReadTimeoutDoesNotCountTheDeferredWork) {
  ExpectDeferredWorkIsNotIdleness(ShortBodyReadConfig());
}

#ifdef AERONET_LINUX
// A zerocopy send arms the deadline of its connection to watch its completion, whatever the next request does: once
// completed, the deadline must not close the connection of a handler that works.
TEST(HttpAsyncHandlers, CompletedZerocopySendDoesNotExposeDeferredWorkToTheKeepAliveTimeout) {
  // Before the server, which waits for the deferred work when destroyed.
  std::atomic_bool started{false};
  std::atomic_bool allow{false};
  HttpServerConfig cfg = ShortKeepAliveConfig();
  cfg.withZerocopyMode(ZerocopyMode::Enabled).withZerocopyMinBytes(1024U);
  test::TestServer server(std::move(cfg));
  server.router().setPath(http::Method::GET, "/large",
                          [](const HttpRequestView&) { return HttpResponse(std::string(64UL * 1024UL, 'x')); });
  server.router().setPath(http::Method::GET, "/work", [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
    co_await req.deferWork([&] {
      started.store(true, std::memory_order_release);
      WaitFor(allow);
    });
    co_return HttpResponse("done");
  });

  test::ClientConnection client(server.port());
  test::sendAll(client.fd(),
                "GET /large HTTP/1.1\r\nHost: x\r\n\r\nGET /work HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  ASSERT_TRUE(WaitFor(started));
  std::this_thread::sleep_for(3 * kShortTimeout);
  allow.store(true, std::memory_order_release);
  const std::string resp = test::recvUntilClosed(client.fd(), std::chrono::seconds{3});
  EXPECT_TRUE(resp.ends_with("\r\n\r\ndone")) << resp.size() << " bytes received";
}
#endif

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

// A stream opened then reset at once frees its slot while its deferred work runs: maxConcurrentStreams did not bound
// the threads of a connection.
TEST(Http2AsyncHandlers, WorkOfResetStreamsCountsAgainstMaxConcurrentStreams) {
  static constexpr int kMaxConcurrentStreams = 10;
  // Before the server, which waits for the deferred work when destroyed.
  std::atomic_bool allow{false};
  std::atomic<int> nbDispatched{0};

  test::TlsHttp2TestServer server;
  server.server.postConfigUpdate(
      [](HttpServerConfig& cfg) { cfg.http2.maxConcurrentStreams = static_cast<uint32_t>(kMaxConcurrentStreams); });
  server.server.resetRouterAndGet().setPath(http::Method::GET, "/slow",
                                            [&](HttpRequestView& req) -> RequestTask<HttpResponse> {
                                              nbDispatched.fetch_add(1, std::memory_order_relaxed);
                                              co_await req.deferWork([&allow] { WaitFor(allow); });
                                              co_return HttpResponse("done");
                                            });
  server.server.router().setPath(http::Method::GET, "/fast",
                                 [](const HttpRequestView&) { return HttpResponse("fast"); });

  test::TlsHttp2Client client(server.port());
  ASSERT_TRUE(client.isConnected());
  for (int streamPos = 0; streamPos < 3 * kMaxConcurrentStreams; ++streamPos) {
    const uint32_t streamId = client.sendAsyncRequest("GET", "/slow");
    ASSERT_NE(streamId, 0U);
    client.connection().sendRstStream(streamId, http2::ErrorCode::Cancel);
  }

  // Flushes the last RST_STREAM. No slot is left: this stream is refused, like the last /slow ones.
  const auto refused = client.waitAndGetResponse(client.sendAsyncRequest("GET", "/fast"), 5000ms);
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->statusCode, 0);
  EXPECT_EQ(nbDispatched.load(std::memory_order_relaxed), kMaxConcurrentStreams);

  // The slots come back once the work completes.
  allow.store(true, std::memory_order_release);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  test::TlsHttp2Client::Response resp;
  while (resp.statusCode != 200 && std::chrono::steady_clock::now() < deadline) {
    resp = client.waitAndGetResponse(client.sendAsyncRequest("GET", "/fast"), 5000ms)
               .value_or(test::TlsHttp2Client::Response{});
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(resp.statusCode, 200);
  EXPECT_EQ(resp.body, "fast");
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

// A request answered by its route timeout is a completed request.
TEST(Http2AsyncHandlers, TimedOutHandlerIsCountedInTheRequestMetrics) {
  // Before the server, which waits for the deferred work when destroyed.
  std::atomic_bool allow{false};
  std::atomic<int> metricsStatus{0};
  test::TlsHttp2TestServer server;
  server.server.server.setMetricsCallback(
      [&metricsStatus](const RequestMetrics& metrics) { metricsStatus.store(metrics.status); });
  server.server.resetRouterAndGet()
      .setPath(http::Method::GET, "/slow",
               [&allow](HttpRequestView& req) -> RequestTask<HttpResponse> {
                 co_await req.deferWork([&allow] { WaitFor(allow); });
                 co_return HttpResponse("late");
               })
      .timeout(50ms);

  test::TlsHttp2Client client(server.port());
  ASSERT_TRUE(client.isConnected());
  const auto resp = client.get("/slow");
  allow.store(true, std::memory_order_release);
  EXPECT_EQ(resp.statusCode, 408);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (metricsStatus.load() == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(metricsStatus.load(), 408);
}

// The last response after a GOAWAY closes the connection right away when it comes from deferred work, like over
// HTTP/1.1: without waiting for the next sweep, that can be far away (a maintenance timer does not always exist).
TEST(Http2AsyncHandlers, LastDeferredResponseAfterGoAwayClosesTheConnectionRightAway) {
  test::TlsHttp2TestServer server;
  server.server.postConfigUpdate([](HttpServerConfig& cfg) {
    cfg.withPollInterval(10s);
    cfg.http2.maxStreamsPerConnection = 1;
  });
  server.server.resetRouterAndGet().setPath(http::Method::GET, "/work",
                                            [](HttpRequestView& req) -> RequestTask<HttpResponse> {
                                              co_await req.deferWork([] { std::this_thread::sleep_for(20ms); });
                                              co_return HttpResponse("done");
                                            });

  test::TlsHttp2Client client(server.port());
  ASSERT_TRUE(client.isConnected());
  const auto resp = client.get("/work");
  EXPECT_EQ(resp.statusCode, 200);
  EXPECT_EQ(resp.body, "done");
  // The socket of the client is non-blocking: each call returns at once while the connection is open.
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  bool closed = false;
  while (!closed && std::chrono::steady_clock::now() < deadline) {
    closed = test::WaitForPeerClose(client.fd(), 100ms);
  }
  EXPECT_TRUE(closed);
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
