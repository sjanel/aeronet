// Micro-benchmark for the HTTP/1.1 client Http1ResponseParser body ownership paths.
//
// On completion, the parser transfers a suitably-sized identity receive or chunked reassembly allocation into
// HttpResponse and installs an equal-capacity empty replacement. These benchmarks cover that steady-state rotation:
// the next keep-alive exchange does not re-grow its scratch buffer, while the completed body avoids a second copy.
// BM_StreamChunkedBody covers a chunked body streamed to a ResponseBodySink instead, read after read.
#include <benchmark/benchmark.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/response-body-sink.hpp"
#include "http1-response-parser.hpp"

namespace aeronet {
namespace {

constexpr std::size_t kMaxResponseBytes = 64UL * 1024UL * 1024UL;

// Build a chunked HTTP/1.1 response whose body is `bodyBytes` bytes split into `chunkBytes`-sized chunks.
std::string MakeChunkedResponse(std::size_t bodyBytes, std::size_t chunkBytes) {
  std::string raw = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nTransfer-Encoding: chunked\r\n\r\n";
  for (std::size_t emitted = 0; emitted < bodyBytes;) {
    const std::size_t take = std::min(chunkBytes, bodyBytes - emitted);
    char hex[16];
    const auto [ptr, ec] = std::to_chars(hex, hex + sizeof(hex), take, 16);
    raw.append(hex, ptr);
    raw.append(http::CRLF);
    raw.append(take, 'x');
    raw.append(http::CRLF);
    emitted += take;
  }
  raw.append("0\r\n\r\n");  // last-chunk marker + trailing CRLF
  return raw;
}

std::string MakeIdentityResponse(std::size_t bodyBytes) {
  std::string raw = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: ";
  raw.append(std::to_string(bodyBytes));
  raw.append("\r\n\r\n");
  raw.append(bodyBytes, 'x');
  return raw;
}

// Build a bodyless response with repeated consumed headers, isolating status/header CRLF scanning without
// response-header storage or body assembly dominating the measurement.
std::string MakeHeadOnlyResponse(std::size_t headerCount, std::size_t valueBytes) {
  std::string raw = "HTTP/1.1 204 No Content\r\n";
  raw.reserve(raw.size() + headerCount * (sizeof("Content-Type: \r\n") - 1U + valueBytes) + 2U);
  for (std::size_t idx = 0; idx < headerCount; ++idx) {
    raw.append("Content-Type: ");
    raw.append(valueBytes, 'x');
    raw.append(http::CRLF);
  }
  raw.append(http::CRLF);
  return raw;
}

void BM_ChunkedTransferBody(benchmark::State& state) {
  std::string response =
      MakeChunkedResponse(static_cast<std::size_t>(state.range(0)), static_cast<std::size_t>(state.range(1)));
  RawChars bodyBuf;
  for (auto _ : state) {
    HttpResponse resp;
    Http1ResponseParser parser(bodyBuf);
    parser.reset(false);
    auto st = parser.parse(response, false, resp, kMaxResponseBytes);
    benchmark::DoNotOptimize(st);
    benchmark::DoNotOptimize(resp.bodyInMemory().data());
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(response.size()));
}
BENCHMARK(BM_ChunkedTransferBody)->Args({256UL * 1024UL, 1024UL})->Args({64UL * 1024UL, 16UL});

void BM_IdentityTransferBody(benchmark::State& state) {
  const std::string response = MakeIdentityResponse(static_cast<std::size_t>(state.range(0)));
  RawChars bodyBuf;
  RawChars receiveBuffer(response.size());
  for (auto _ : state) {
    receiveBuffer.assign(response);
    HttpResponse resp;
    Http1ResponseParser parser(bodyBuf);
    parser.reset(false);
    auto status = parser.parse(receiveBuffer, false, resp, kMaxResponseBytes);
    benchmark::DoNotOptimize(status);
    benchmark::DoNotOptimize(resp.bodyInMemory().data());
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(response.size()));
}
BENCHMARK(BM_IdentityTransferBody)->Arg(256UL * 1024UL)->Arg((1UL << 20U) + 123U);

void BM_ResponseHeadScan(benchmark::State& state) {
  std::string response =
      MakeHeadOnlyResponse(static_cast<std::size_t>(state.range(0)), static_cast<std::size_t>(state.range(1)));
  RawChars bodyBuf;
  for (auto _ : state) {
    HttpResponse resp;
    Http1ResponseParser parser(bodyBuf);
    parser.reset(false);
    auto st = parser.parse(response, false, resp, kMaxResponseBytes);
    benchmark::DoNotOptimize(st);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(response.size()));
}
BENCHMARK(BM_ResponseHeadScan)->Args({32UL, 64UL});

// Counts the delivered body bytes.
class CountingSink final : public ResponseBodySink {
 public:
  bool onHead([[maybe_unused]] const HttpResponse& head, [[maybe_unused]] const ResponseBodyInfo& body) override {
    return true;
  }

  bool onBody(std::string_view chunk) override {
    benchmark::DoNotOptimize(chunk.data());
    bytes += chunk.size();
    return true;
  }

  std::size_t bytes{0};
};

// Streams a chunked body of state.range(0) bytes in chunks of state.range(1) bytes like the HTTP/1.1 engine does for a
// response delivered to a ResponseBodySink: each 16 KiB read is appended to the receive buffer and parsed, the body it
// carries is delivered to the sink (a virtual call), then the consumed bytes are dropped from the buffer.
void BM_StreamChunkedBody(benchmark::State& state) {
  static constexpr std::size_t kReadBytes = 16384;
  const std::string response =
      MakeChunkedResponse(static_cast<std::size_t>(state.range(0)), static_cast<std::size_t>(state.range(1)));
  const std::string_view raw(response);
  RawChars bodyBuf;
  RawChars receive(2 * kReadBytes);
  CountingSink sink;
  ResponseBodySink* pSink = &sink;
  benchmark::DoNotOptimize(pSink);  // a sink of unknown type, as for the engine
  for ([[maybe_unused]] auto _ : state) {
    HttpResponse resp;
    Http1ResponseParser parser(bodyBuf);
    parser.reset(false, /*streamHead=*/true);
    receive.clear();
    for (std::size_t off = 0;;) {
      const std::string_view read = raw.substr(off, kReadBytes);
      off += read.size();
      receive.append(read);
      auto st = parser.parse(receive, false, resp, kMaxResponseBytes);
      if (st == Http1ResponseParser::Status::HeadComplete) {
        parser.startStreaming();
        st = parser.parse(receive, false, resp, kMaxResponseBytes);
      }
      while (st == Http1ResponseParser::Status::BodyPiece) {
        pSink->onBody(parser.streamedBody(receive));
        st = parser.parse(receive, false, resp, kMaxResponseBytes);
      }
      if (parser.streaming()) {
        parser.discardStreamed(receive);
      }
      if (st != Http1ResponseParser::Status::NeedMore) {
        break;
      }
    }
  }
  if (sink.bytes != static_cast<std::size_t>(state.iterations()) * static_cast<std::size_t>(state.range(0))) {
    state.SkipWithError("streamed body size mismatch");
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(response.size()));
}
BENCHMARK(BM_StreamChunkedBody)
    ->ArgsProduct({{1L << 20}, {16, 256, 1024, 16384}})
    ->ArgNames({"body", "chunk"})
    ->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
