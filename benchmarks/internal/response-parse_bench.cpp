// Micro-benchmark for the HTTP/1.1 client ResponseParser chunked-body reassembly path.
//
// The parser borrows its chunked reassembly buffer (HttpClient::bodyBuffer()) instead of owning it, so a
// keep-alive connection streaming chunked responses reuses a single allocation across exchanges. This
// benchmark contrasts reusing the borrowed buffer (steady state) against re-allocating it on every exchange
// (the previous owned-member behaviour) to size the win. Length-framed bodies never touch the buffer, so
// only the chunked path is measured here.
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

// 256 KiB body in 1 KiB chunks (256 chunks): forces the reassembly buffer to grow to the full body size.
const std::string kChunkedResponse = MakeChunkedResponse(256UL * 1024UL, 1024UL);

// Reuse one borrowed reassembly buffer across every parse (current behaviour: HttpClient::bodyBuffer()).
void BM_ChunkedReuseBuffer(benchmark::State& state) {
  RawChars bodyBuf;  // persists across iterations, as HttpClient::bodyBuffer() does across exchanges
  for (auto _ : state) {
    HttpResponse resp;
    ResponseParser parser(bodyBuf);
    parser.reset(false);
    auto st = parser.parse(kChunkedResponse, false, resp, kMaxResponseBytes);
    benchmark::DoNotOptimize(st);
    benchmark::DoNotOptimize(resp.bodyInMemory().data());
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kChunkedResponse.size()));
}
BENCHMARK(BM_ChunkedReuseBuffer);

// Re-allocate the reassembly buffer on every parse (the previous owned-member behaviour).
void BM_ChunkedFreshBuffer(benchmark::State& state) {
  for (auto _ : state) {
    RawChars bodyBuf;  // fresh allocation every iteration
    HttpResponse resp;
    ResponseParser parser(bodyBuf);
    parser.reset(false);
    auto st = parser.parse(kChunkedResponse, false, resp, kMaxResponseBytes);
    benchmark::DoNotOptimize(st);
    benchmark::DoNotOptimize(resp.bodyInMemory().data());
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(kChunkedResponse.size()));
}
BENCHMARK(BM_ChunkedFreshBuffer);

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
