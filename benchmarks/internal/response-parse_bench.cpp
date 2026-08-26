// Micro-benchmark for the HTTP/1.1 client ResponseParser chunked-body reassembly path.
//
// On completion, the parser transfers its chunked reassembly allocation into HttpResponse and installs an
// equal-capacity empty replacement. This benchmark covers that steady-state rotation: the next keep-alive
// exchange does not re-grow its scratch buffer, while the completed body avoids a second large copy.
#include <benchmark/benchmark.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/raw-chars.hpp"
#include "response-parser.hpp"

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
    ResponseParser parser(bodyBuf);
    parser.reset(false);
    auto st = parser.parse(response, false, resp, kMaxResponseBytes);
    benchmark::DoNotOptimize(st);
    benchmark::DoNotOptimize(resp.bodyInMemory().data());
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(response.size()));
}
BENCHMARK(BM_ChunkedTransferBody)->Args({256UL * 1024UL, 1024UL})->Args({64UL * 1024UL, 16UL});

void BM_ResponseHeadScan(benchmark::State& state) {
  std::string response =
      MakeHeadOnlyResponse(static_cast<std::size_t>(state.range(0)), static_cast<std::size_t>(state.range(1)));
  RawChars bodyBuf;
  for (auto _ : state) {
    HttpResponse resp;
    ResponseParser parser(bodyBuf);
    parser.reset(false);
    auto st = parser.parse(response, false, resp, kMaxResponseBytes);
    benchmark::DoNotOptimize(st);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(response.size()));
}
BENCHMARK(BM_ResponseHeadScan)->Args({32UL, 64UL});

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
