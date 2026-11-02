// Benchmark the parsing of chunk-size lines (aeronet/chunk-size-line.hpp), and the hexadecimal digit decoders.
//
// Walking a chunked body is a dependency chain: the size of a chunk gives the position of the next chunk-size line.
// BM_ParseChunkSizeLines walks bodies of small and larger chunks with the syntax and limits of the server (strict,
// limits from its configuration) and of the client (lenient, no limit). BM_HexDigitChain and BM_HexDigitPercentDecode
// compare FromHexDigit (lookup table) and FromHexDigitLowLatency (arithmetic) on such a chain and on independent
// digits: the table lookup delays each step of the chain, the arithmetic is slower on independent digits. Hence the
// former for percent-decoding, the latter for chunk sizes.

#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <format>
#include <random>
#include <string>
#include <string_view>

#include "aeronet/char-hexadecimal-converter.hpp"
#include "aeronet/chunk-size-line.hpp"
#include "aeronet/compiler-config.hpp"

namespace {

using aeronet::http::ChunkSizeLine;
using aeronet::http::ChunkSizeSyntax;

constexpr std::size_t kNbChunks = 4096;

// A chunked body of kNbChunks chunks of chunkSize bytes, followed by its last-chunk.
std::string MakeChunkedBody(std::size_t chunkSize) {
  std::string body;
  for (std::size_t chunkPos = 0; chunkPos < kNbChunks; ++chunkPos) {
    body += std::format("{:x}\r\n", chunkSize);
    body.append(chunkSize, 'x');
    body += "\r\n";
  }
  body += "0\r\n\r\n";
  return body;
}

// Returns the total size of the chunks of 'body', up to its last-chunk.
AERONET_ALWAYS_INLINE std::size_t WalkChunks(std::string_view body, std::size_t maxLineSize, std::size_t maxChunkSize,
                                             ChunkSizeSyntax syntax) {
  std::size_t pos = 0;
  std::size_t total = 0;
  while (true) {
    const ChunkSizeLine line = aeronet::http::ParseChunkSizeLine(body.data() + pos, body.data() + body.size(),
                                                                 maxLineSize, maxChunkSize, syntax);
    if (line.state != ChunkSizeLine::State::Complete || line.chunkSize == 0) {
      return total;
    }
    total += line.chunkSize;
    pos += line.size + line.chunkSize + 2U;
  }
}

// The server's limits are only known at run time.
AERONET_NOINLINE std::size_t WalkChunksAsServer(std::string_view body, std::size_t maxLineSize,
                                                std::size_t maxChunkSize) {
  return WalkChunks(body, maxLineSize, maxChunkSize, ChunkSizeSyntax::Strict);
}

AERONET_NOINLINE std::size_t WalkChunksAsClient(std::string_view body) {
  return WalkChunks(body, aeronet::http::kNoChunkSizeLineLimit, aeronet::http::kNoChunkSizeLineLimit,
                    ChunkSizeSyntax::Lenient);
}

// Argument 0: the chunk size, argument 1: 0 for the server, 1 for the client.
void BM_ParseChunkSizeLines(benchmark::State& state) {
  const std::string body = MakeChunkedBody(static_cast<std::size_t>(state.range(0)));
  const bool asClient = state.range(1) != 0;
  std::size_t maxLineSize = std::size_t{8} << 10U;
  std::size_t maxChunkSize = std::size_t{256} << 20U;
  benchmark::DoNotOptimize(maxLineSize);
  benchmark::DoNotOptimize(maxChunkSize);
  for (auto _ : state) {
    benchmark::DoNotOptimize(asClient ? WalkChunksAsClient(body) : WalkChunksAsServer(body, maxLineSize, maxChunkSize));
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kNbChunks));
}

BENCHMARK(BM_ParseChunkSizeLines)->ArgNames({"chunk", "client"})->ArgsProduct({{16, 1024}, {0, 1}});

// Returns the total size of the chunks of 'body', reading only the hexadecimal digits of their lines.
template <auto HexDigitFn>
AERONET_NOINLINE std::size_t WalkHexDigitChain(std::string_view body) {
  std::size_t pos = 0;
  std::size_t total = 0;
  while (true) {
    std::size_t chunkSize = 0;
    for (int8_t digit = HexDigitFn(body[pos]); digit >= 0; digit = HexDigitFn(body[++pos])) {
      chunkSize = (chunkSize << 4U) | static_cast<std::size_t>(digit);
    }
    if (chunkSize == 0) {
      return total;
    }
    total += chunkSize;
    pos += chunkSize + 4U;  // the CRLF of the line and the one after the data
  }
}

template <auto HexDigitFn>
void BM_HexDigitChain(benchmark::State& state) {
  const std::string body = MakeChunkedBody(16);
  for (auto _ : state) {
    benchmark::DoNotOptimize(WalkHexDigitChain<HexDigitFn>(body));
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kNbChunks));
}

BENCHMARK(BM_HexDigitChain<aeronet::FromHexDigit>);
BENCHMARK(BM_HexDigitChain<aeronet::FromHexDigitLowLatency>);

// Decodes 'encoded', made of %XX triplets only, into 'out'. Returns the number of decoded bytes (0 if invalid).
template <auto HexDigitFn>
AERONET_NOINLINE std::size_t PercentDecode(std::string_view encoded, char* out) {
  char* pOut = out;
  for (std::size_t pos = 0; pos + 2U < encoded.size(); pos += 3U) {
    const int8_t high = HexDigitFn(encoded[pos + 1U]);
    const int8_t low = HexDigitFn(encoded[pos + 2U]);
    if (high < 0 || low < 0) {
      return 0;
    }
    *pOut++ = static_cast<char>((static_cast<unsigned>(high) << 4U) | static_cast<unsigned>(low));
  }
  return static_cast<std::size_t>(pOut - out);
}

// Random bytes, all percent-encoded (letters and digits mixed like in encoded UTF-8 text).
template <auto HexDigitFn>
void BM_HexDigitPercentDecode(benchmark::State& state) {
  static constexpr std::size_t kNbBytes = 4096;
  std::mt19937 rng(42);
  std::string encoded;
  for (std::size_t bytePos = 0; bytePos < kNbBytes; ++bytePos) {
    encoded += std::format("%{:02X}", rng() & 0xFFU);
  }
  std::string decoded(kNbBytes, '\0');
  for (auto _ : state) {
    benchmark::DoNotOptimize(PercentDecode<HexDigitFn>(encoded, decoded.data()));
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(kNbBytes));
}

BENCHMARK(BM_HexDigitPercentDecode<aeronet::FromHexDigit>);
BENCHMARK(BM_HexDigitPercentDecode<aeronet::FromHexDigitLowLatency>);

}  // namespace

BENCHMARK_MAIN();
