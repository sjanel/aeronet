// HTTP/2 frame parsing and writing micro-benchmarks.
// Measures ParseFrameHeader, ParseDataFrame, ParseHeadersFrame,
// WriteDataFrame, WriteHeadersFrame, and bulk processFrames throughput.
#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

#include "aeronet/hpack.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/raw-bytes.hpp"

namespace aeronet::http2 {
namespace {

// ---------------------------------------------------------------------------
// ParseFrameHeader: 9-byte parse throughput
// ---------------------------------------------------------------------------

void BM_ParseFrameHeader(benchmark::State& state) {
  // Construct a valid 9-byte frame header (DATA, 256 bytes, stream 1)
  RawBytes buf;
  WriteFrame(buf, FrameType::Data, FrameFlags::DataEndStream, /*streamId=*/1, /*payloadSize=*/256);
  auto data = std::span<const std::byte>(buf.begin(), FrameHeader::kSize);

  for ([[maybe_unused]] auto iter : state) {
    auto header = ParseFrameHeader(data);
    benchmark::DoNotOptimize(header);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(FrameHeader::kSize));
}
BENCHMARK(BM_ParseFrameHeader);

// ---------------------------------------------------------------------------
// ParseDataFrame: various payload sizes
// ---------------------------------------------------------------------------

void BM_ParseDataFrame(benchmark::State& state) {
  const auto payloadSize = static_cast<std::size_t>(state.range(0));

  RawBytes buf;
  // Write a DATA frame with the specified payload size
  [[maybe_unused]] auto totalWritten =
      WriteDataFrame(buf, /*streamId=*/1, std::span<const std::byte>(reinterpret_cast<const std::byte*>(""), 0),
                     /*endStream=*/false);
  // Overwrite with actual payload
  buf.clear();
  std::string payload(payloadSize, 'X');
  auto payloadBytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()), payload.size());
  WriteDataFrame(buf, /*streamId=*/1, payloadBytes, /*endStream=*/true);

  // Parse just the payload portion (after 9-byte header)
  FrameHeader fh{};
  fh.length = static_cast<uint32_t>(payloadSize);
  fh.type = FrameType::Data;
  fh.flags = FrameFlags::DataEndStream;
  fh.streamId = 1;

  auto payloadSpan = std::span<const std::byte>(buf.begin() + FrameHeader::kSize, payloadSize);

  for ([[maybe_unused]] auto iter : state) {
    DataFrame df{};
    auto result = ParseDataFrame(fh, payloadSpan, df);
    benchmark::DoNotOptimize(result);
    benchmark::DoNotOptimize(df);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(payloadSize));
}
BENCHMARK(BM_ParseDataFrame)->Arg(64)->Arg(1024)->Arg(16384)->Arg(65536);

// ---------------------------------------------------------------------------
// ParseHeadersFrame: with pre-encoded HPACK header block
// ---------------------------------------------------------------------------

void BM_ParseHeadersFrame(benchmark::State& state) {
  constexpr std::array<http::HeaderView, 5> headers{{
      {":method", "GET"},
      {":path", "/"},
      {":scheme", "https"},
      {":authority", "example.com"},
      {"accept", "text/html"},
  }};

  HpackEncoder encoder;
  RawBytes hpackBlock;
  for (const auto& hv : headers) {
    encoder.encode(hpackBlock, hv.name, hv.value);
  }

  // Build a HEADERS frame
  RawBytes frameBuf;
  WriteHeadersFrameWithPriority(frameBuf, /*streamId=*/1,
                                std::span<const std::byte>(hpackBlock.begin(), hpackBlock.size()),
                                /*streamDependency=*/0, /*weight=*/16, /*exclusive=*/false,
                                /*endStream=*/true, /*endHeaders=*/true);

  FrameHeader fh = ParseFrameHeader(std::span<const std::byte>(frameBuf.begin(), FrameHeader::kSize));
  auto payload = std::span<const std::byte>(frameBuf.begin() + FrameHeader::kSize, fh.length);

  for ([[maybe_unused]] auto iter : state) {
    HeadersFrame hf{};
    auto result = ParseHeadersFrame(fh, payload, hf);
    benchmark::DoNotOptimize(result);
    benchmark::DoNotOptimize(hf);
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(fh.length));
}
BENCHMARK(BM_ParseHeadersFrame);

// ---------------------------------------------------------------------------
// WriteDataFrame: various payload sizes
// ---------------------------------------------------------------------------

void BM_WriteDataFrame(benchmark::State& state) {
  const auto payloadSize = static_cast<std::size_t>(state.range(0));
  std::string payload(payloadSize, 'Y');
  auto payloadBytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()), payload.size());

  for ([[maybe_unused]] auto iter : state) {
    RawBytes buf;
    auto written = WriteDataFrame(buf, /*streamId=*/1, payloadBytes, /*endStream=*/false);
    benchmark::DoNotOptimize(written);
    benchmark::DoNotOptimize(buf.data());
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(payloadSize + 9));
}
BENCHMARK(BM_WriteDataFrame)->Arg(64)->Arg(1024)->Arg(16384)->Arg(65536);

// ---------------------------------------------------------------------------
// WriteHeadersFrame: encode + write
// ---------------------------------------------------------------------------

void BM_WriteHeadersFrame(benchmark::State& state) {
  constexpr std::array<http::HeaderView, 5> headers{{
      {":method", "GET"},
      {":path", "/api/v1/bench"},
      {":scheme", "https"},
      {":authority", "example.com"},
      {"content-type", "application/json"},
  }};

  // Pre-encode the HPACK block
  HpackEncoder templateEncoder;
  RawBytes hpackBlock;
  for (const auto& hv : headers) {
    templateEncoder.encode(hpackBlock, hv.name, hv.value);
  }
  auto hpackSpan = std::span<const std::byte>(hpackBlock.begin(), hpackBlock.size());

  for ([[maybe_unused]] auto iter : state) {
    RawBytes buf;
    auto written = WriteHeadersFrameWithPriority(buf, /*streamId=*/1, hpackSpan,
                                                 /*streamDependency=*/0, /*weight=*/16, /*exclusive=*/false,
                                                 /*endStream=*/true, /*endHeaders=*/true);
    benchmark::DoNotOptimize(written);
    benchmark::DoNotOptimize(buf.data());
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(hpackBlock.size() + 9 + 5));
}
BENCHMARK(BM_WriteHeadersFrame);

// ---------------------------------------------------------------------------
// WriteWindowUpdateFrame
// ---------------------------------------------------------------------------

void BM_WriteWindowUpdateFrame(benchmark::State& state) {
  for ([[maybe_unused]] auto iter : state) {
    RawBytes buf;
    auto written = WriteWindowUpdateFrame(buf, /*streamId=*/1, /*windowSizeIncrement=*/65535);
    benchmark::DoNotOptimize(written);
    benchmark::DoNotOptimize(buf.data());
  }
}
BENCHMARK(BM_WriteWindowUpdateFrame);

// ---------------------------------------------------------------------------
// WriteSettingsFrame
// ---------------------------------------------------------------------------

void BM_WriteSettingsFrame(benchmark::State& state) {
  std::array<SettingsEntry, 4> entries{{
      {SettingsParameter::HeaderTableSize, 4096},
      {SettingsParameter::MaxConcurrentStreams, 100},
      {SettingsParameter::InitialWindowSize, 65535},
      {SettingsParameter::MaxFrameSize, 16384},
  }};

  for ([[maybe_unused]] auto iter : state) {
    RawBytes buf;
    auto written = WriteSettingsFrame(buf, entries);
    benchmark::DoNotOptimize(written);
    benchmark::DoNotOptimize(buf.data());
  }
}
BENCHMARK(BM_WriteSettingsFrame);

// ---------------------------------------------------------------------------
// WritePingFrame
// ---------------------------------------------------------------------------

void BM_WritePingFrame(benchmark::State& state) {
  PingFrame ping{};
  std::memset(ping.opaqueData, 0x42, sizeof(ping.opaqueData));
  ping.isAck = false;

  for ([[maybe_unused]] auto iter : state) {
    RawBytes buf;
    auto written = WritePingFrame(buf, ping);
    benchmark::DoNotOptimize(written);
    benchmark::DoNotOptimize(buf.data());
  }
}
BENCHMARK(BM_WritePingFrame);

// ---------------------------------------------------------------------------
// Bulk frame buffer parse: simulate N DATA frames in a contiguous buffer
// ---------------------------------------------------------------------------

void BM_BulkParseDataFrames(benchmark::State& state) {
  const int frameCount = static_cast<int>(state.range(0));
  constexpr std::size_t kPayloadSize = 128;
  std::string payload(kPayloadSize, 'Z');
  auto payloadBytes = std::span<const std::byte>(reinterpret_cast<const std::byte*>(payload.data()), payload.size());

  // Build contiguous buffer with N DATA frames
  RawBytes bulkBuf;
  for (int ii = 0; ii < frameCount; ++ii) {
    WriteDataFrame(bulkBuf, /*streamId=*/1, payloadBytes, /*endStream=*/(ii == frameCount - 1));
  }

  auto bufSpan = std::span<const std::byte>(bulkBuf.begin(), bulkBuf.size());

  for ([[maybe_unused]] auto iter : state) {
    std::size_t pos = 0;
    int parsed = 0;
    while (pos + FrameHeader::kSize <= bufSpan.size()) {
      auto fh = ParseFrameHeader(bufSpan.subspan(pos, FrameHeader::kSize));
      pos += FrameHeader::kSize;
      if (pos + fh.length > bufSpan.size()) {
        break;
      }
      DataFrame df{};
      (void)ParseDataFrame(fh, bufSpan.subspan(pos, fh.length), df);
      benchmark::DoNotOptimize(df);
      pos += fh.length;
      ++parsed;
    }
    benchmark::DoNotOptimize(parsed);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * frameCount);
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(bulkBuf.size()));
}
BENCHMARK(BM_BulkParseDataFrames)->Arg(10)->Arg(100)->Arg(1000);

}  // namespace
}  // namespace aeronet::http2

BENCHMARK_MAIN();
