// Growth of RawChars buffers (RawBytesBase::reallocUp), whose small blocks are grown into a new block (malloc, copy,
// free) instead of being reallocated, and larger ones reallocated:
//   * BM_PushBack / BM_Append  - a buffer grown from empty byte by byte, or by chunks, to its final size.
//   * BM_ResponseLike          - the buffer of an HTTP response: a small initial capacity (status line and Date),
//                                grown by a few headers and a body, then waiting in a queue of in-flight responses.
//   * BM_ManyLive              - many live buffers growing together (their neighbors stay allocated).
// Each case also runs with 8 threads, like the event loops of a multi-threaded server.
// Then the operations of buffers which do not grow (inlined in the callers, no call into the library):
//   * BM_MoveRotate            - ownership transfers, like responses moving through queues: a move construction, two
//                                move assignments and the destruction of a moved-from buffer.
//   * BM_Swap                  - the input buffer of the event loop lent to a connection (swap), filled by a request,
//                                consumed and given back (swap).
//   * BM_ConsumeWhole          - a request appended to an input buffer and then consumed whole (erase_front).

#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/raw-chars.hpp"

namespace aeronet {
namespace {

constexpr std::array<char, 4096> kChunk{};

void BM_PushBack(benchmark::State& state) {
  const auto finalSize = static_cast<std::size_t>(state.range(0));
  for ([[maybe_unused]] auto iter : state) {
    RawChars buf;
    for (std::size_t pos = 0; pos < finalSize; ++pos) {
      buf.push_back(static_cast<char>(pos));
    }
    benchmark::DoNotOptimize(buf.data());
  }
}
BENCHMARK(BM_PushBack)->Arg(64)->Arg(256)->Arg(1024)->ThreadRange(1, 8);

void BM_Append(benchmark::State& state) {
  const auto finalSize = static_cast<std::size_t>(state.range(0));
  const auto chunkSize = static_cast<std::size_t>(state.range(1));
  for ([[maybe_unused]] auto iter : state) {
    RawChars buf;
    for (std::size_t size = 0; size < finalSize; size += chunkSize) {
      buf.append(kChunk.data(), chunkSize);
    }
    benchmark::DoNotOptimize(buf.data());
  }
}
BENCHMARK(BM_Append)
    ->Args({128, 16})
    ->Args({256, 24})
    ->Args({1024, 24})
    ->Args({4096, 100})
    ->Args({32768, 1024})
    ->ThreadRange(1, 8);

void BM_ResponseLike(benchmark::State& state) {
  static constexpr std::size_t kInFlight = 64;
  static constexpr std::size_t kHeaderSizes[] = {30, 41, 28};
  static constexpr std::size_t kFinalizationSize = 40;  // content-length, server, connection...

  const auto bodySize = static_cast<std::size_t>(state.range(0));
  std::vector<RawChars> inFlight(kInFlight);
  std::size_t slot = 0;
  for ([[maybe_unused]] auto iter : state) {
    RawChars buf(71);  // HttpResponse::kHttpResponseMinInitialCapacity
    buf.setSize(67);
    for (std::size_t headerSize : kHeaderSizes) {
      buf.append(kChunk.data(), headerSize);
    }
    buf.append(kChunk.data(), bodySize);
    buf.append(kChunk.data(), kFinalizationSize);
    benchmark::DoNotOptimize(buf.data());
    inFlight[slot] = std::move(buf);
    slot = (slot + 1) % kInFlight;
  }
}
BENCHMARK(BM_ResponseLike)->Arg(16)->Arg(200)->Arg(1000)->ThreadRange(1, 8);

void BM_ManyLive(benchmark::State& state) {
  static constexpr std::size_t kChunkSize = 32;

  const auto nbBuffers = static_cast<std::size_t>(state.range(0));
  const auto finalSize = static_cast<std::size_t>(state.range(1));
  for ([[maybe_unused]] auto iter : state) {
    std::vector<RawChars> bufs(nbBuffers);
    for (std::size_t size = 0; size < finalSize; size += kChunkSize) {
      for (auto& buf : bufs) {
        buf.append(kChunk.data(), kChunkSize);
      }
    }
    benchmark::DoNotOptimize(bufs.data());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(nbBuffers));
}
BENCHMARK(BM_ManyLive)->Args({256, 512})->Args({64, 2048})->ThreadRange(1, 8);

void BM_MoveRotate(benchmark::State& state) {
  static constexpr std::size_t kNbBuffers = 64;

  std::vector<RawChars> bufs;
  bufs.reserve(kNbBuffers);
  for (std::size_t idx = 0; idx < kNbBuffers; ++idx) {
    bufs.emplace_back(kChunk.data(), 100);
  }
  std::size_t pos = 0;
  for ([[maybe_unused]] auto iter : state) {
    RawChars& first = bufs[pos];
    RawChars& second = bufs[(pos + 1) % kNbBuffers];
    RawChars tmp(std::move(first));
    first = std::move(second);
    second = std::move(tmp);
    benchmark::DoNotOptimize(bufs.data());
    pos = (pos + 1) % kNbBuffers;
  }
}
BENCHMARK(BM_MoveRotate);

void BM_Swap(benchmark::State& state) {
  static constexpr std::size_t kRequestSize = 80;

  RawChars connectionBuf;
  RawChars lentBuf(4096);
  // Like the buffers of a connection and of its event loop: the compiler cannot tell them apart. Two buffers it knows
  // to be distinct get their sizes swapped by 16-byte moves, which stall after the 8-byte store of a size.
  RawChars* pConnectionBuf = &connectionBuf;
  RawChars* pLentBuf = &lentBuf;
  benchmark::DoNotOptimize(pConnectionBuf);
  benchmark::DoNotOptimize(pLentBuf);
  for ([[maybe_unused]] auto iter : state) {
    pConnectionBuf->swap(*pLentBuf);  // lent to the connection for its read
    pConnectionBuf->append(kChunk.data(), kRequestSize);
    benchmark::DoNotOptimize(pConnectionBuf->data());
    pConnectionBuf->erase_front(pConnectionBuf->size());
    pConnectionBuf->swap(*pLentBuf);  // given back once served
  }
}
BENCHMARK(BM_Swap);

void BM_ConsumeWhole(benchmark::State& state) {
  static constexpr std::size_t kRequestSize = 80;

  RawChars buf(4096);
  for ([[maybe_unused]] auto iter : state) {
    buf.append(kChunk.data(), kRequestSize);
    benchmark::DoNotOptimize(buf.data());
    buf.erase_front(buf.size());
  }
}
BENCHMARK(BM_ConsumeWhole);

}  // namespace
}  // namespace aeronet

int main(int argc, char** argv) {
  // A server is multi-threaded: glibc then locks the arena in realloc, unlike in the thread cache of malloc and free.
  std::thread([] {}).join();
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
