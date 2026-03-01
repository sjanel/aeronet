// HPACK encoder/decoder micro-benchmarks.
// Measures hot paths: decode, encode, findHeader, Huffman, dynamic table ops.
#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

#include "aeronet/hpack.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/raw-bytes.hpp"

namespace aeronet::http2 {
namespace {

// ---------------------------------------------------------------------------
// Helpers: build synthetic HPACK-encoded header blocks at various sizes
// ---------------------------------------------------------------------------

// Encode a set of headers into a raw HPACK block using the encoder.
RawBytes EncodeHeaderBlock(std::span<const http::HeaderView> headers) {
  HpackEncoder encoder;
  RawBytes out;
  for (const auto& hv : headers) {
    encoder.encode(out, hv.name, hv.value);
  }
  return out;
}

// Small: 5 typical request pseudo-headers + a few regular headers
constexpr std::array<http::HeaderView, 5> kSmallHeaders{{
    {":method", "GET"},
    {":path", "/api/users/123"},
    {":scheme", "https"},
    {":authority", "example.com"},
    {"accept", "application/json"},
}};

// Medium: 20 headers mixing indexed and literal
constexpr std::array<http::HeaderView, 20> kMediumHeaders{{
    {":method", "POST"},
    {":path", "/api/v2/resources"},
    {":scheme", "https"},
    {":authority", "bench.example.com"},
    {"content-type", "application/json"},
    {"accept", "application/json"},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", "en-US"},
    {"authorization", "Bearer dummy-token-value"},
    {"cache-control", "no-cache"},
    {"user-agent", "aeronet-bench/1.0"},
    {"x-request-id", "aaaabbbb-cccc-dddd-eeee-ffff00001111"},
    {"x-correlation-id", "11112222-3333-4444-5555-666677778888"},
    {"content-length", "256"},
    {"cookie", "session=dummy_cookie; theme=dark"},
    {"referer", "https://bench.example.com/dashboard"},
    {"origin", "https://bench.example.com"},
    {"x-forwarded-for", "192.168.1.100"},
    {"x-real-ip", "10.0.0.42"},
    {"if-none-match", "W/\"abc123\""},
}};

// Large: 50 headers — simulates header-heavy workloads (proxies, CDN)
std::array<http::HeaderView, 50> BuildLargeHeaders() {
  // We need storage that outlives the function — use static buffers.
  static std::array<std::string, 50> names;
  static std::array<std::string, 50> values;
  std::array<http::HeaderView, 50> hdrs{};

  // First 4 are pseudo-headers
  names[0] = ":method";
  values[0] = "GET";
  names[1] = ":path";
  values[1] = "/api/benchmark/large-header-stress";
  names[2] = ":scheme";
  values[2] = "https";
  names[3] = ":authority";
  values[3] = "large-header.bench.example.com";

  for (std::size_t ii = 4; ii < 50; ++ii) {
    names[ii] = "x-bench-hdr-" + std::to_string(ii);
    values[ii] = std::string(128, static_cast<char>('a' + (ii % 26)));
  }
  for (std::size_t ii = 0; ii < 50; ++ii) {
    hdrs[ii] = {names[ii], values[ii]};
  }
  return hdrs;
}

const auto kLargeHeaders = BuildLargeHeaders();

// Pre-encoded blocks (built once, reused across iterations)
const auto kSmallBlock = EncodeHeaderBlock(kSmallHeaders);
const auto kMediumBlock = EncodeHeaderBlock(kMediumHeaders);
const auto kLargeBlock = EncodeHeaderBlock(kLargeHeaders);

std::span<const std::byte> AsBytes(const RawBytes& rb) { return {rb.begin(), rb.size()}; }

// ---------------------------------------------------------------------------
// Decode benchmarks
// ---------------------------------------------------------------------------

void BM_HpackDecodeSmall(benchmark::State& state) {
  auto block = AsBytes(kSmallBlock);
  for ([[maybe_unused]] auto iter : state) {
    HpackDecoder decoder;
    auto result = decoder.decode(block);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_HpackDecodeSmall);

void BM_HpackDecodeMedium(benchmark::State& state) {
  auto block = AsBytes(kMediumBlock);
  for ([[maybe_unused]] auto iter : state) {
    HpackDecoder decoder;
    auto result = decoder.decode(block);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_HpackDecodeMedium);

void BM_HpackDecodeLarge(benchmark::State& state) {
  auto block = AsBytes(kLargeBlock);
  for ([[maybe_unused]] auto iter : state) {
    HpackDecoder decoder;
    auto result = decoder.decode(block);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_HpackDecodeLarge);

// Stateful decode: decoder persists across iterations (dynamic table builds up)
void BM_HpackDecodeSmallStateful(benchmark::State& state) {
  auto block = AsBytes(kSmallBlock);
  HpackDecoder decoder;
  for ([[maybe_unused]] auto iter : state) {
    auto result = decoder.decode(block);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_HpackDecodeSmallStateful);

void BM_HpackDecodeMediumStateful(benchmark::State& state) {
  auto block = AsBytes(kMediumBlock);
  HpackDecoder decoder;
  for ([[maybe_unused]] auto iter : state) {
    auto result = decoder.decode(block);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_HpackDecodeMediumStateful);

void BM_HpackDecodeLargeStateful(benchmark::State& state) {
  auto block = AsBytes(kLargeBlock);
  HpackDecoder decoder;
  for ([[maybe_unused]] auto iter : state) {
    auto result = decoder.decode(block);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_HpackDecodeLargeStateful);

// ---------------------------------------------------------------------------
// Encode benchmarks
// ---------------------------------------------------------------------------

void BM_HpackEncodeSmall(benchmark::State& state) {
  for ([[maybe_unused]] auto iter : state) {
    HpackEncoder encoder;
    RawBytes out;
    for (const auto& hv : kSmallHeaders) {
      encoder.encode(out, hv.name, hv.value);
    }
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_HpackEncodeSmall);

void BM_HpackEncodeMedium(benchmark::State& state) {
  for ([[maybe_unused]] auto iter : state) {
    HpackEncoder encoder;
    RawBytes out;
    for (const auto& hv : kMediumHeaders) {
      encoder.encode(out, hv.name, hv.value);
    }
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_HpackEncodeMedium);

void BM_HpackEncodeLarge(benchmark::State& state) {
  for ([[maybe_unused]] auto iter : state) {
    HpackEncoder encoder;
    RawBytes out;
    for (const auto& hv : kLargeHeaders) {
      encoder.encode(out, hv.name, hv.value);
    }
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_HpackEncodeLarge);

// Stateful encode: encoder persists across iterations (dynamic table builds up)
void BM_HpackEncodeSmallStateful(benchmark::State& state) {
  HpackEncoder encoder;
  for ([[maybe_unused]] auto iter : state) {
    RawBytes out;
    for (const auto& hv : kSmallHeaders) {
      encoder.encode(out, hv.name, hv.value);
    }
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_HpackEncodeSmallStateful);

void BM_HpackEncodeMediumStateful(benchmark::State& state) {
  HpackEncoder encoder;
  for ([[maybe_unused]] auto iter : state) {
    RawBytes out;
    for (const auto& hv : kMediumHeaders) {
      encoder.encode(out, hv.name, hv.value);
    }
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_HpackEncodeMediumStateful);

void BM_HpackEncodeLargeStateful(benchmark::State& state) {
  HpackEncoder encoder;
  for ([[maybe_unused]] auto iter : state) {
    RawBytes out;
    for (const auto& hv : kLargeHeaders) {
      encoder.encode(out, hv.name, hv.value);
    }
    benchmark::DoNotOptimize(out.data());
  }
}
BENCHMARK(BM_HpackEncodeLargeStateful);

// ---------------------------------------------------------------------------
// findHeader benchmark (the identified bottleneck)
// ---------------------------------------------------------------------------

// Parameterized: dynamic table size 0, 10, 50, 100
void BM_HpackFindHeader(benchmark::State& state) {
  const int dynTableEntries = static_cast<int>(state.range(0));

  HpackEncoder encoder;
  // Populate dynamic table
  RawBytes dummy;
  for (int ii = 0; ii < dynTableEntries; ++ii) {
    std::string nm = "x-dyn-" + std::to_string(ii);
    std::string vl = "value-" + std::to_string(ii);
    encoder.encode(dummy, nm, vl);
  }

  // Search for headers that are: in static table, in dynamic table, and not found
  static constexpr std::array<http::HeaderView, 4> kQueries{{
      {":method", "GET"},                        // static full match
      {"content-type", "application/json"},      // static name-only
      {"x-dyn-0", "value-0"},                    // dynamic full match (if populated)
      {"x-not-found", "no-match-anywhere-val"},  // miss
  }};

  std::size_t qi = 0;
  for ([[maybe_unused]] auto iter : state) {
    const auto& qh = kQueries[qi % kQueries.size()];
    auto result = encoder.findHeader(qh.name, qh.value);
    benchmark::DoNotOptimize(result);
    ++qi;
  }
  state.counters["dyn_entries"] = static_cast<double>(dynTableEntries);
}
BENCHMARK(BM_HpackFindHeader)->Arg(0)->Arg(10)->Arg(50)->Arg(100);

// ---------------------------------------------------------------------------
// Encode-decode round-trip
// ---------------------------------------------------------------------------

void BM_HpackRoundTrip(benchmark::State& state) {
  const auto headerCount = static_cast<std::size_t>(state.range(0));
  auto headers =
      headerCount <= kSmallHeaders.size()    ? std::span<const http::HeaderView>(kSmallHeaders.data(), headerCount)
      : headerCount <= kMediumHeaders.size() ? std::span<const http::HeaderView>(kMediumHeaders.data(), headerCount)
                                             : std::span<const http::HeaderView>(kLargeHeaders.data(), headerCount);

  for ([[maybe_unused]] auto iter : state) {
    HpackEncoder encoder;
    RawBytes encoded;
    for (const auto& hv : headers) {
      encoder.encode(encoded, hv.name, hv.value);
    }
    HpackDecoder decoder;
    auto result = decoder.decode(AsBytes(encoded));
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_HpackRoundTrip)->Arg(5)->Arg(20)->Arg(50);

// ---------------------------------------------------------------------------
// Dynamic table add + eviction cycle
// ---------------------------------------------------------------------------

void BM_HpackDynamicTableAddEvict(benchmark::State& state) {
  HpackDynamicTable table(4096);  // 4KB default
  int idx = 0;
  for ([[maybe_unused]] auto iter : state) {
    std::string nm = "x-h-" + std::to_string(idx % 200);
    std::string vl = std::string(64, static_cast<char>('a' + (idx % 26)));
    table.add(nm, vl);
    benchmark::DoNotOptimize(table.currentSize());
    ++idx;
  }
}
BENCHMARK(BM_HpackDynamicTableAddEvict);

}  // namespace
}  // namespace aeronet::http2

BENCHMARK_MAIN();
