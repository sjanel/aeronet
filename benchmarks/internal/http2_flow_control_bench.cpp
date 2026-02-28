// HTTP/2 flow control and stream management micro-benchmarks.
// Measures window consume/increase cycles, stream creation/destruction,
// and connection-level flow control processing.
#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "aeronet/flat-hash-map.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/http2-stream.hpp"
#include "aeronet/raw-bytes.hpp"

namespace aeronet::http2 {
namespace {

// ---------------------------------------------------------------------------
// Stream window consume/increase cycle
// ---------------------------------------------------------------------------

void BM_StreamWindowConsume(benchmark::State& state) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  // Simulate open state by receiving headers
  [[maybe_unused]] auto headersResult = stream.onRecvHeaders(/*endStream=*/false);

  for ([[maybe_unused]] auto iter : state) {
    // Consume 1024 bytes from recv window
    bool ok = stream.consumeRecvWindow(1024);
    benchmark::DoNotOptimize(ok);
    // Restore the window
    (void)stream.increaseRecvWindow(1024);
  }
}
BENCHMARK(BM_StreamWindowConsume);

void BM_StreamWindowIncreaseSend(benchmark::State& state) {
  Http2Stream stream(1, kDefaultInitialWindowSize);
  [[maybe_unused]] auto headersResult = stream.onRecvHeaders(/*endStream=*/false);

  for ([[maybe_unused]] auto iter : state) {
    // Consume from send window
    bool ok = stream.consumeSendWindow(4096);
    benchmark::DoNotOptimize(ok);
    // WINDOW_UPDATE restores it
    auto ec = stream.increaseSendWindow(4096);
    benchmark::DoNotOptimize(ec);
  }
}
BENCHMARK(BM_StreamWindowIncreaseSend);

// ---------------------------------------------------------------------------
// Stream state transitions
// ---------------------------------------------------------------------------

void BM_StreamStateTransitions(benchmark::State& state) {
  for ([[maybe_unused]] auto iter : state) {
    Http2Stream stream(1, kDefaultInitialWindowSize);
    auto ec1 = stream.onRecvHeaders(/*endStream=*/false);
    benchmark::DoNotOptimize(ec1);
    auto ec2 = stream.onSendHeaders(/*endStream=*/false);
    benchmark::DoNotOptimize(ec2);
    auto ec3 = stream.onRecvData(/*endStream=*/true);
    benchmark::DoNotOptimize(ec3);
    auto ec4 = stream.onSendData(/*endStream=*/true);
    benchmark::DoNotOptimize(ec4);
  }
}
BENCHMARK(BM_StreamStateTransitions);

// ---------------------------------------------------------------------------
// Stream map insert/erase (flat_hash_map throughput)
// ---------------------------------------------------------------------------

void BM_StreamMapInsertErase(benchmark::State& state) {
  const auto count = static_cast<uint32_t>(state.range(0));
  for ([[maybe_unused]] auto iter : state) {
    flat_hash_map<uint32_t, Http2Stream> streams;
    // Insert count streams (odd IDs = client-initiated)
    for (uint32_t ii = 1; ii <= count * 2; ii += 2) {
      streams.emplace(ii, Http2Stream(ii, kDefaultInitialWindowSize));
    }
    // Erase them all
    for (uint32_t ii = 1; ii <= count * 2; ii += 2) {
      streams.erase(ii);
    }
    benchmark::DoNotOptimize(streams.size());
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * count * 2);
}
BENCHMARK(BM_StreamMapInsertErase)->Arg(10)->Arg(100)->Arg(500);

// ---------------------------------------------------------------------------
// WriteWindowUpdateFrame throughput (the auto-update hot path)
// ---------------------------------------------------------------------------

void BM_WindowUpdateFrameWrite(benchmark::State& state) {
  RawBytes buf;
  for ([[maybe_unused]] auto iter : state) {
    buf.clear();
    // Connection-level + stream-level (2 frames per DATA received)
    WriteWindowUpdateFrame(buf, /*streamId=*/0, /*windowSizeIncrement=*/16384);
    WriteWindowUpdateFrame(buf, /*streamId=*/1, /*windowSizeIncrement=*/16384);
    benchmark::DoNotOptimize(buf.data());
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * 2);
}
BENCHMARK(BM_WindowUpdateFrameWrite);

// ---------------------------------------------------------------------------
// Full connection SETTINGS exchange (processInput hot path)
// ---------------------------------------------------------------------------

void BM_ConnectionSettingsExchange(benchmark::State& state) {
  // Build client preface + SETTINGS frame
  RawBytes clientInput;
  // Client magic
  clientInput.append(reinterpret_cast<const std::byte*>(kConnectionPreface.data()), kConnectionPreface.size());
  // Client SETTINGS (empty = use defaults)
  WriteSettingsFrame(clientInput, {});

  auto inputSpan = std::span<const std::byte>(clientInput.begin(), clientInput.size());

  for ([[maybe_unused]] auto iter : state) {
    Http2Connection conn(Http2Config{}, /*isServer=*/true);
    auto result = conn.processInput(inputSpan);
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_ConnectionSettingsExchange);

// ---------------------------------------------------------------------------
// Connection: process N DATA frames (simulated fast path)
// ---------------------------------------------------------------------------

void BM_ConnectionProcessDataFrames(benchmark::State& state) {
  const int frameCount = static_cast<int>(state.range(0));
  constexpr std::size_t kPayloadSize = 128;

  // Build a complete h2 session: client preface + SETTINGS + SETTINGS_ACK + HEADERS + N DATA frames
  RawBytes clientInput;

  // Client preface
  clientInput.append(reinterpret_cast<const std::byte*>(kConnectionPreface.data()), kConnectionPreface.size());

  // Client SETTINGS
  WriteSettingsFrame(clientInput, {});

  // SETTINGS ACK
  WriteSettingsAckFrame(clientInput);

  // HEADERS frame for stream 1 (need HPACK-encoded pseudo-headers)
  HpackEncoder encoder;
  RawBytes hpackBlock;
  encoder.encode(hpackBlock, ":method", "POST");
  encoder.encode(hpackBlock, ":path", "/benchmark");
  encoder.encode(hpackBlock, ":scheme", "http");
  encoder.encode(hpackBlock, ":authority", "localhost");

  WriteHeadersFrameWithPriority(clientInput, /*streamId=*/1,
                                std::span<const std::byte>(hpackBlock.begin(), hpackBlock.size()),
                                /*streamDependency=*/0, /*weight=*/16, /*exclusive=*/false,
                                /*endStream=*/false, /*endHeaders=*/true);

  // N DATA frames
  std::string payload(kPayloadSize, 'D');
  auto payloadBytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()), payload.size());
  for (int ii = 0; ii < frameCount; ++ii) {
    WriteDataFrame(clientInput, /*streamId=*/1, payloadBytes, /*endStream=*/(ii == frameCount - 1));
  }

  auto fullInput = std::span<const std::byte>(clientInput.begin(), clientInput.size());

  for ([[maybe_unused]] auto iter : state) {
    Http2Connection conn(Http2Config{}, /*isServer=*/true);
    conn.setOnHeadersDecoded([](uint32_t, const HeadersViewMap&, bool) {});
    conn.setOnData([](uint32_t, std::span<const std::byte>, bool) {});
    auto result = conn.processInput(fullInput);
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * frameCount);
}
BENCHMARK(BM_ConnectionProcessDataFrames)->Arg(10)->Arg(100)->Arg(500);

}  // namespace
}  // namespace aeronet::http2

BENCHMARK_MAIN();
