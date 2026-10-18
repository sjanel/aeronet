// Cost of constructing the messages built on every exchange: an HttpResponse returned by a handler (directly or through
// HttpRequestView::makeResponse(), with the global headers of the server) and an HttpRequest made by
// HttpClient::makeRequest(), without body, with a copied body, and with a moved (captured) body.
#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"

#ifdef AERONET_ENABLE_HTTP_CLIENT
#include "aeronet/http-method.hpp"
#include "aeronet/http-request.hpp"
#endif

namespace aeronet {

namespace {

constexpr std::string_view kServerGlobalHeaders = "server: aeronet\r\n";

}  // namespace

// HttpResponse befriends this class (also used by the unit tests) to reach the constructors of
// HttpRequestView::makeResponse().
class HttpResponseTest {
 public:
  static HttpResponse MakeResponse(std::string_view body) {
    return {0, http::StatusCodeOK, kServerGlobalHeaders, body, http::ContentTypeTextPlain, PreparedOptions()};
  }

  static HttpResponse MakeResponse(std::string&& body) {
    return {0,
            http::StatusCodeOK,
            kServerGlobalHeaders,
            HttpPayload(std::move(body)),
            http::ContentTypeTextPlain,
            PreparedOptions()};
  }

 private:
  // Options of a response made by HttpRequestView::makeResponse() for a GET request, without compression.
  static HttpMessage::Options PreparedOptions() {
    HttpMessage::Options opts;
    opts.setPrepared();
    return opts;
  }
};

#ifdef AERONET_ENABLE_HTTP_CLIENT
// HttpRequest befriends this class (also used by the unit tests) to reach the constructors of
// HttpClient::makeRequest().
class HttpRequestTest {
 public:
  static HttpRequest MakeRequest(http::Method method) {
    return {0, method, kUrl, kClientGlobalHeaders, RequestOptions()};
  }

  static HttpRequest MakeRequest(http::Method method, std::string_view body) {
    return {0, method, kUrl, kClientGlobalHeaders, RequestOptions(), body, http::ContentTypeTextPlain};
  }

  static HttpRequest MakeRequest(http::Method method, std::string&& body) {
    return {0,
            method,
            kUrl,
            kClientGlobalHeaders,
            RequestOptions(),
            HttpPayload(std::move(body)),
            http::ContentTypeTextPlain};
  }

 private:
  static constexpr std::string_view kUrl = "http://example.com:8080/api/items?id=42";
  static constexpr std::string_view kClientGlobalHeaders = "user-agent: aeronet\r\n";

  static HttpMessage::Options RequestOptions() {
    HttpMessage::Options opts;
    opts.setHttpRequest();
    opts.setPrepared();
    return opts;
  }
};
#endif

namespace {

std::string MakeBody(const benchmark::State& state) {
  return std::string(static_cast<std::size_t>(state.range(0)), 'x');
}

// ------------------------------------------------------------------
// HttpResponse
// ------------------------------------------------------------------

void BM_ResponseStatusOnly(benchmark::State& state) {
  for ([[maybe_unused]] auto iter : state) {
    HttpResponse resp(http::StatusCodeNotFound);
    benchmark::DoNotOptimize(&resp);
  }
}

void BM_ResponseCopiedBody(benchmark::State& state) {
  const std::string body = MakeBody(state);
  for ([[maybe_unused]] auto iter : state) {
    HttpResponse resp(http::StatusCodeOK, std::string_view(body));
    benchmark::DoNotOptimize(&resp);
  }
}

// makeResponse(body): global headers and prepared options.
void BM_ResponsePreparedCopiedBody(benchmark::State& state) {
  const std::string body = MakeBody(state);
  for ([[maybe_unused]] auto iter : state) {
    HttpResponse resp = HttpResponseTest::MakeResponse(std::string_view(body));
    benchmark::DoNotOptimize(&resp);
  }
}

// The handler builds a std::string body (included in the timing), then sets it with body(std::string&&).
void BM_ResponseMovedBodySetter(benchmark::State& state) {
  const std::string body = MakeBody(state);
  for ([[maybe_unused]] auto iter : state) {
    HttpResponse resp(http::StatusCodeOK);
    resp.body(std::string(body));
    benchmark::DoNotOptimize(&resp);
  }
}

// Same, but the std::string body is given to the constructor.
void BM_ResponseMovedBodyConstructor(benchmark::State& state) {
  const std::string body = MakeBody(state);
  for ([[maybe_unused]] auto iter : state) {
    HttpResponse resp(http::StatusCodeOK, std::string(body));
    benchmark::DoNotOptimize(&resp);
  }
}

// makeResponse(std::string&&): global headers and prepared options.
void BM_ResponsePreparedMovedBody(benchmark::State& state) {
  const std::string body = MakeBody(state);
  for ([[maybe_unused]] auto iter : state) {
    HttpResponse resp = HttpResponseTest::MakeResponse(std::string(body));
    benchmark::DoNotOptimize(&resp);
  }
}

#ifdef AERONET_ENABLE_HTTP_CLIENT
// ------------------------------------------------------------------
// HttpRequest
// ------------------------------------------------------------------

void BM_RequestNoBody(benchmark::State& state) {
  for ([[maybe_unused]] auto iter : state) {
    HttpRequest req = HttpRequestTest::MakeRequest(http::Method::GET);
    benchmark::DoNotOptimize(&req);
  }
}

void BM_RequestCopiedBody(benchmark::State& state) {
  const std::string body = MakeBody(state);
  for ([[maybe_unused]] auto iter : state) {
    HttpRequest req = HttpRequestTest::MakeRequest(http::Method::POST, std::string_view(body));
    benchmark::DoNotOptimize(&req);
  }
}

void BM_RequestMovedBodySetter(benchmark::State& state) {
  const std::string body = MakeBody(state);
  for ([[maybe_unused]] auto iter : state) {
    HttpRequest req = HttpRequestTest::MakeRequest(http::Method::POST);
    req.body(std::string(body));
    benchmark::DoNotOptimize(&req);
  }
}

void BM_RequestMovedBodyConstructor(benchmark::State& state) {
  const std::string body = MakeBody(state);
  for ([[maybe_unused]] auto iter : state) {
    HttpRequest req = HttpRequestTest::MakeRequest(http::Method::POST, std::string(body));
    benchmark::DoNotOptimize(&req);
  }
}
#endif

// A small body (as most API responses) and a larger one (where moving instead of copying matters).
void BodySizes(benchmark::Benchmark* bench) {
  for (const int64_t size : {16, 4096}) {
    bench->Arg(size);
  }
}

}  // namespace

BENCHMARK(BM_ResponseStatusOnly);
BENCHMARK(BM_ResponseCopiedBody)->Apply(BodySizes);
// No body: makeResponse(statusCode).
BENCHMARK(BM_ResponsePreparedCopiedBody)->Arg(0)->Apply(BodySizes);
BENCHMARK(BM_ResponseMovedBodySetter)->Apply(BodySizes);
BENCHMARK(BM_ResponseMovedBodyConstructor)->Apply(BodySizes);
BENCHMARK(BM_ResponsePreparedMovedBody)->Apply(BodySizes);

#ifdef AERONET_ENABLE_HTTP_CLIENT
BENCHMARK(BM_RequestNoBody);
BENCHMARK(BM_RequestCopiedBody)->Apply(BodySizes);
BENCHMARK(BM_RequestMovedBodySetter)->Apply(BodySizes);
BENCHMARK(BM_RequestMovedBodyConstructor)->Apply(BodySizes);
#endif

}  // namespace aeronet

BENCHMARK_MAIN();
