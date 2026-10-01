// Cost of carrying an HttpResponse body payload from the handler to the connection output buffer.
// Compares body kinds (inline copy, captured std::string, static view) along the server path:
//   handler builds the response -> std::optional hop (middleware / async state) -> HTTP/1 finalization ->
//   HttpMessageData moved into the connection output buffer -> buffers read by the transport.
#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/concatenated-headers.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http-version.hpp"

namespace aeronet {

// HttpResponse befriends this class (also used by the unit tests) to reach the HTTP/1 finalization of the server.
class HttpResponseTest {
 public:
  static HttpMessageData Finalize(HttpResponse& resp, const ConcatenatedHeaders& globalHeaders,
                                  std::size_t minCapturedBodySize) {
    return resp.finalizeForHttp1(kDateHeader, http::HTTP_1_1, HttpResponse::Options{}, &globalHeaders,
                                 minCapturedBodySize);
  }

 private:
  static constexpr const char* kDateHeader = "Thu, 01 Jan 1970 00:00:00 GMT";
};

namespace {

enum class BodyKind : std::uint8_t { Inline, CapturedString, Static };

std::string_view BodyKindName(BodyKind kind) {
  switch (kind) {
    case BodyKind::Inline:
      return "inline";
    case BodyKind::CapturedString:
      return "string";
    default:
      return "static";
  }
}

// What a typical handler returns: a couple of headers and a body.
HttpResponse BuildResponse(BodyKind kind, std::string_view body) {
  HttpResponse resp(http::StatusCodeOK);
  resp.headerAddLine("cache-control", "no-cache");
  resp.headerAddLine("x-request-id", "0123456789abcdef");
  switch (kind) {
    case BodyKind::Inline:
      resp.body(body, http::ContentTypeTextPlain);
      break;
    case BodyKind::CapturedString:
      resp.body(std::string(body), http::ContentTypeTextPlain);
      break;
    default:
      resp.bodyStatic(body, http::ContentTypeTextPlain);
      break;
  }
  return resp;
}

// The body bytes, living for the whole benchmark (so also usable as a static body).
std::string_view MakeBody(std::size_t size, std::string& storage) {
  storage.assign(size, 'x');
  return storage;
}

void SetLabelAndSizes(benchmark::State& state, BodyKind kind) {
  state.SetLabel(std::string(BodyKindName(kind)));
  state.counters["sizeofResponse"] = static_cast<double>(sizeof(HttpResponse));
  state.counters["sizeofMessageData"] = static_cast<double>(sizeof(HttpMessageData));
}

void BM_HandlerToOutputBuffer(benchmark::State& state) {
  const auto kind = static_cast<BodyKind>(state.range(0));
  std::string storage;
  const std::string_view body = MakeBody(static_cast<std::size_t>(state.range(1)), storage);

  const HttpServerConfig config;
  HttpMessageData outBuffer;

  for ([[maybe_unused]] auto iter : state) {
    std::optional<HttpResponse> handlerResult(BuildResponse(kind, body));
    HttpResponse resp(std::move(*handlerResult));
    handlerResult.reset();

    outBuffer = HttpResponseTest::Finalize(resp, config.globalHeaders, config.minCapturedBodySize);

    benchmark::DoNotOptimize(outBuffer.firstBuffer().data());
    benchmark::DoNotOptimize(outBuffer.secondBuffer().data());
    outBuffer.clear();
  }
  state.SetItemsProcessed(state.iterations());
  SetLabelAndSizes(state, kind);
}

// One move construction (into an optional) and one move assignment back, per iteration.
void BM_MoveResponse(benchmark::State& state) {
  const auto kind = static_cast<BodyKind>(state.range(0));
  std::string storage;
  const std::string_view body = MakeBody(static_cast<std::size_t>(state.range(1)), storage);

  HttpResponse resp = BuildResponse(kind, body);
  std::optional<HttpResponse> holder;

  for ([[maybe_unused]] auto iter : state) {
    holder.emplace(std::move(resp));
    benchmark::DoNotOptimize(&*holder);
    resp = std::move(*holder);
    holder.reset();
    benchmark::DoNotOptimize(&resp);
  }
  state.SetItemsProcessed(state.iterations());
  SetLabelAndSizes(state, kind);
}

void BodyArgs(benchmark::Benchmark* bench) {
  for (const auto kind : {BodyKind::Inline, BodyKind::CapturedString, BodyKind::Static}) {
    // 16 bytes: captured bodies below minCapturedBodySize are copied inline at finalization.
    for (const int64_t size : {16, 4096}) {
      bench->Args({static_cast<int64_t>(kind), size});
    }
  }
}

}  // namespace

BENCHMARK(BM_HandlerToOutputBuffer)->Apply(BodyArgs);
BENCHMARK(BM_MoveResponse)->Apply(BodyArgs);

}  // namespace aeronet

BENCHMARK_MAIN();
