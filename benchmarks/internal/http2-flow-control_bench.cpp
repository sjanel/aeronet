// HTTP/2 flow control and stream management micro-benchmarks.
// Measures window consume/increase cycles, stream creation/destruction,
// and connection-level flow control processing.
#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/flat-hash-map.hpp"
#include "aeronet/hpack.hpp"
#include "aeronet/http-headers-view.hpp"
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-payload.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-config.hpp"
#include "aeronet/http2-connection.hpp"
#include "aeronet/http2-event-sink.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/http2-stream.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/sv-to-sv-map.hpp"
#include "aeronet/vector.hpp"

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
// Connection: queue owned DATA and expose its frame fragments
// ---------------------------------------------------------------------------

void BM_ConnectionQueueOwnedDataFragments(benchmark::State& state) {
  const auto payloadSize = static_cast<std::size_t>(state.range(0));

  RawBytes clientInput;
  clientInput.append(reinterpret_cast<const std::byte*>(kConnectionPreface.data()), kConnectionPreface.size());
  WriteSettingsFrame(clientInput, {});
  WriteSettingsAckFrame(clientInput);

  HpackEncoder encoder;
  RawBytes hpackBlock;
  encoder.encode(hpackBlock, ":method", "GET");
  encoder.encode(hpackBlock, ":path", "/benchmark");
  encoder.encode(hpackBlock, ":scheme", "http");
  encoder.encode(hpackBlock, ":authority", "localhost");
  WriteHeadersFrameWithPriority(clientInput, /*streamId=*/1,
                                std::span<const std::byte>(hpackBlock.begin(), hpackBlock.size()),
                                /*streamDependency=*/0, /*weight=*/16, /*exclusive=*/false,
                                /*endStream=*/true, /*endHeaders=*/true);

  vector<std::string_view> fragments;
  for ([[maybe_unused]] auto iter : state) {
    state.PauseTiming();
    {
      Http2Connection connection(Http2Config{}, /*isServer=*/true);
      auto input = std::span<const std::byte>(clientInput.begin(), clientInput.size());
      while (!input.empty()) {
        const auto result = connection.processInput(input);
        input = input.subspan(result.bytesConsumed);
      }
      connection.onOutputWritten(connection.pendingOutputSize());

      auto headerError = connection.sendHeaders(/*streamId=*/1, http::StatusCodeOK, HeadersView{}, /*endStream=*/false);
      benchmark::DoNotOptimize(headerError);
      connection.onOutputWritten(connection.pendingOutputSize());

      RawBytes payload(payloadSize);
      payload.setSize(payloadSize);
      state.ResumeTiming();

      auto dataError = connection.sendData(/*streamId=*/1, std::move(payload), /*endStream=*/true);
      connection.getPendingOutputFragments(fragments);
      benchmark::DoNotOptimize(dataError);
      benchmark::DoNotOptimize(fragments.size());
      benchmark::DoNotOptimize(fragments.data());
      connection.onOutputWritten(connection.pendingOutputSize());
      benchmark::ClobberMemory();

      state.PauseTiming();
    }
    state.ResumeTiming();
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(payloadSize));
}
BENCHMARK(BM_ConnectionQueueOwnedDataFragments)->Arg(1024)->Arg(16384)->Arg(65535);

// ---------------------------------------------------------------------------
// Connection: send a captured response body, copied or from its storage
// ---------------------------------------------------------------------------

// fromStorage=0 copies the body (what HTTP/2 did for captured bodies), fromStorage=1 hands over its storage.
void BM_ConnectionSendCapturedBody(benchmark::State& state) {
  const auto bodySize = static_cast<std::size_t>(state.range(0));
  const bool fromStorage = state.range(1) != 0;

  RawBytes clientInput;
  clientInput.append(reinterpret_cast<const std::byte*>(kConnectionPreface.data()), kConnectionPreface.size());
  WriteSettingsFrame(clientInput, {});
  WriteSettingsAckFrame(clientInput);

  HpackEncoder encoder;
  RawBytes hpackBlock;
  encoder.encode(hpackBlock, ":method", "GET");
  encoder.encode(hpackBlock, ":path", "/benchmark");
  encoder.encode(hpackBlock, ":scheme", "http");
  encoder.encode(hpackBlock, ":authority", "localhost");
  WriteHeadersFrameWithPriority(clientInput, /*streamId=*/1,
                                std::span<const std::byte>(hpackBlock.begin(), hpackBlock.size()),
                                /*streamDependency=*/0, /*weight=*/16, /*exclusive=*/false,
                                /*endStream=*/true, /*endHeaders=*/true);

  vector<std::string_view> fragments;
  for ([[maybe_unused]] auto iter : state) {
    state.PauseTiming();
    {
      Http2Connection connection(Http2Config{}, /*isServer=*/true);
      auto input = std::span<const std::byte>(clientInput.begin(), clientInput.size());
      while (!input.empty()) {
        const auto result = connection.processInput(input);
        input = input.subspan(result.bytesConsumed);
      }
      connection.onOutputWritten(connection.pendingOutputSize());

      auto headerError = connection.sendHeaders(/*streamId=*/1, http::StatusCodeOK, HeadersView{}, /*endStream=*/false);
      benchmark::DoNotOptimize(headerError);
      connection.onOutputWritten(connection.pendingOutputSize());

      // The storage of a response with a captured body: its head, followed by the embedded payload.
      HttpMessageData storage(RawChars("HTTP/1.1 200 OK\r\n"), HttpPayload(std::string(bodySize, 'x')));
      const auto body = std::as_bytes(std::span<const char>(storage.secondBuffer()));
      state.ResumeTiming();

      auto dataError = fromStorage ? connection.sendData(/*streamId=*/1, std::move(storage), body, /*endStream=*/true)
                                   : connection.sendData(/*streamId=*/1, body, /*endStream=*/true);
      connection.getPendingOutputFragments(fragments);
      benchmark::DoNotOptimize(dataError);
      benchmark::DoNotOptimize(fragments.size());
      benchmark::DoNotOptimize(fragments.data());
      benchmark::ClobberMemory();

      // The connection and the storage are released untimed, whichever owns the storage.
      state.PauseTiming();
    }
    state.ResumeTiming();
  }
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(bodySize));
}
BENCHMARK(BM_ConnectionSendCapturedBody)
    ->ArgsProduct({{1024, 16384, 65535}, {0, 1}})
    ->ArgNames({"size", "fromStorage"});

// ---------------------------------------------------------------------------
// Connection: batch small response HEADERS and DATA into one output fragment
// ---------------------------------------------------------------------------

void BM_ConnectionBatchSmallResponses(benchmark::State& state) {
  const auto responseCount = static_cast<uint32_t>(state.range(0));

  RawBytes clientInput;
  clientInput.append(reinterpret_cast<const std::byte*>(kConnectionPreface.data()), kConnectionPreface.size());
  WriteSettingsFrame(clientInput, {});
  WriteSettingsAckFrame(clientInput);

  HpackEncoder encoder;
  for (uint32_t streamIndex = 0; streamIndex < responseCount; ++streamIndex) {
    RawBytes hpackBlock;
    encoder.encode(hpackBlock, ":method", "GET");
    encoder.encode(hpackBlock, ":path", "/ping");
    encoder.encode(hpackBlock, ":scheme", "https");
    encoder.encode(hpackBlock, ":authority", "localhost");
    WriteHeadersFrameWithPriority(clientInput, (streamIndex * 2U) + 1U,
                                  std::span<const std::byte>(hpackBlock.begin(), hpackBlock.size()), 0, 16, false, true,
                                  true);
  }

  static constexpr std::array<std::byte, 4> kBody{std::byte{'p'}, std::byte{'o'}, std::byte{'n'}, std::byte{'g'}};
  vector<std::string_view> fragments;
  for ([[maybe_unused]] auto iteration : state) {
    state.PauseTiming();
    Http2Connection connection(Http2Config(), true);
    auto input = std::span<const std::byte>(clientInput.begin(), clientInput.size());
    while (!input.empty()) {
      const auto result = connection.processInput(input);
      input = input.subspan(result.bytesConsumed);
    }
    connection.onOutputWritten(connection.pendingOutputSize());
    state.ResumeTiming();

    for (uint32_t streamIndex = 0; streamIndex < responseCount; ++streamIndex) {
      const uint32_t streamId = (streamIndex * 2U) + 1U;
      benchmark::DoNotOptimize(connection.sendHeaders(streamId, http::StatusCodeOK, HeadersView{}, false));
      RawBytes owner(kBody.size());
      owner.unchecked_append(kBody);
      benchmark::DoNotOptimize(connection.sendData(streamId, std::move(owner), true));
    }
    connection.getPendingOutputFragments(fragments);
    benchmark::DoNotOptimize(fragments.data());
    benchmark::DoNotOptimize(fragments.size());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * responseCount);
}
BENCHMARK(BM_ConnectionBatchSmallResponses)->Arg(10)->Arg(100);

// ---------------------------------------------------------------------------
// Connection: responses with owned bodies, written per batch or after each response
// ---------------------------------------------------------------------------

// Each response is its HEADERS frame (in the output buffer) and an owned body above the inline copy size (an output
// block). drainEach=1 writes the output after each response, drainEach=0 once for the whole batch.
void BM_ConnectionResponsesWithOwnedBodies(benchmark::State& state) {
  const auto responseCount = static_cast<uint32_t>(state.range(0));
  const bool drainEach = state.range(1) != 0;
  constexpr std::size_t kBodySize = 1024;

  RawBytes clientInput;
  clientInput.append(reinterpret_cast<const std::byte*>(kConnectionPreface.data()), kConnectionPreface.size());
  WriteSettingsFrame(clientInput, {});
  WriteSettingsAckFrame(clientInput);
  // Enough connection-level credit for all the bodies.
  WriteWindowUpdateFrame(clientInput, 0, 1U << 20);

  HpackEncoder encoder;
  for (uint32_t streamIndex = 0; streamIndex < responseCount; ++streamIndex) {
    RawBytes hpackBlock;
    encoder.encode(hpackBlock, ":method", "GET");
    encoder.encode(hpackBlock, ":path", "/body");
    encoder.encode(hpackBlock, ":scheme", "https");
    encoder.encode(hpackBlock, ":authority", "localhost");
    WriteHeadersFrameWithPriority(clientInput, (streamIndex * 2U) + 1U,
                                  std::span<const std::byte>(hpackBlock.begin(), hpackBlock.size()), 0, 16, false, true,
                                  true);
  }

  vector<RawBytes> bodies;
  vector<std::string_view> fragments;
  const auto writeOutput = [&fragments](Http2Connection& connection) {
    connection.getPendingOutputFragments(fragments);
    benchmark::DoNotOptimize(fragments.data());
    connection.onOutputWritten(connection.pendingOutputSize());
  };
  for ([[maybe_unused]] auto iteration : state) {
    state.PauseTiming();
    {
      Http2Connection connection(Http2Config(), true);
      auto input = std::span<const std::byte>(clientInput.begin(), clientInput.size());
      while (!input.empty()) {
        const auto result = connection.processInput(input);
        input = input.subspan(result.bytesConsumed);
      }
      connection.onOutputWritten(connection.pendingOutputSize());
      bodies.clear();
      for (uint32_t streamIndex = 0; streamIndex < responseCount; ++streamIndex) {
        RawBytes& body = bodies.emplace_back(kBodySize);
        body.setSize(kBodySize);
      }
      state.ResumeTiming();

      for (uint32_t streamIndex = 0; streamIndex < responseCount; ++streamIndex) {
        const uint32_t streamId = (streamIndex * 2U) + 1U;
        benchmark::DoNotOptimize(connection.sendHeaders(streamId, http::StatusCodeOK, HeadersView{}, false));
        benchmark::DoNotOptimize(connection.sendData(streamId, std::move(bodies[streamIndex]), 0, kBodySize, true));
        if (drainEach) {
          writeOutput(connection);
        }
      }
      if (!drainEach) {
        writeOutput(connection);
      }
      benchmark::ClobberMemory();

      state.PauseTiming();
    }
    state.ResumeTiming();
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * responseCount);
}
BENCHMARK(BM_ConnectionResponsesWithOwnedBodies)
    ->ArgsProduct({{10, 100}, {0, 1}})
    ->ArgNames({"responses", "drainEach"});

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
    std::byte* pData =
        PrepareDataFrameGetStartPtr(clientInput, /*streamId=*/1, static_cast<uint32_t>(payloadBytes.size()),
                                    /*endStream=*/(ii == frameCount - 1));
    Copy(payloadBytes.data(), payloadBytes.size(), pData);
  }

  auto fullInput = std::span<const std::byte>(clientInput.begin(), clientInput.size());

  for ([[maybe_unused]] auto iter : state) {
    Http2Connection conn(Http2Config{}, /*isServer=*/true);
    auto result = conn.processInput(fullInput);
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * frameCount);
}
BENCHMARK(BM_ConnectionProcessDataFrames)->Arg(10)->Arg(100)->Arg(500);

// ---------------------------------------------------------------------------
// Connection with a sink: streams answered from its callback, DATA received on an open stream
// ---------------------------------------------------------------------------

// Answers each complete request from its onHeadersDecoded callback with a bodyless response, which closes the stream,
// as a protocol handler does for a synchronous handler.
class AnsweringSink final : public EventSink {
 public:
  explicit AnsweringSink(Http2Connection& connection) noexcept : _connection(connection) {
    _connection.setEventSink(this);
  }

  AnsweringSink(const AnsweringSink&) = delete;
  AnsweringSink(AnsweringSink&&) noexcept = delete;
  AnsweringSink& operator=(const AnsweringSink&) = delete;
  AnsweringSink& operator=(AnsweringSink&&) noexcept = delete;

  ~AnsweringSink() override { _connection.setEventSink(nullptr); }

  void onHeadersDecoded(uint32_t streamId, [[maybe_unused]] DecodedHeaders& decodedHeaders, bool endStream) override {
    if (endStream) {
      benchmark::DoNotOptimize(_connection.sendHeaders(streamId, http::StatusCodeNoContent, HeadersView{}, true));
    }
  }
  void onData([[maybe_unused]] uint32_t streamId, std::span<const std::byte> data,
              [[maybe_unused]] bool endStream) override {
    benchmark::DoNotOptimize(data.data());
  }
  void onStreamReset([[maybe_unused]] uint32_t streamId, [[maybe_unused]] ErrorCode errorCode) override {}
  void onStreamClosed([[maybe_unused]] uint32_t streamId) override {}
  void onGoAway([[maybe_unused]] uint32_t lastStreamId, [[maybe_unused]] ErrorCode errorCode,
                [[maybe_unused]] std::string_view debugData) override {}
  void onWindowUpdate([[maybe_unused]] uint32_t streamId, [[maybe_unused]] uint32_t increment) override {}

 private:
  Http2Connection& _connection;
};

// The fields of a small GET request: the first block indexes them in the dynamic table, the next ones refer to them.
struct RequestFieldBlocks {
  RequestFieldBlocks() {
    HpackEncoder encoder;
    for (RawBytes* pBlock : {&first, &next}) {
      encoder.encode(*pBlock, ":method", "GET");
      encoder.encode(*pBlock, ":path", "/ping");
      encoder.encode(*pBlock, ":scheme", "https");
      encoder.encode(*pBlock, ":authority", "localhost");
    }
  }

  RawBytes first;
  RawBytes next;
};

void WriteHeadersFrame(std::byte* pFrame, std::size_t blockSize, uint32_t streamId, bool endStream) {
  WriteFrameHeader(pFrame, FrameHeader(static_cast<uint32_t>(blockSize), FrameType::Headers,
                                       ComputeHeaderFrameFlags(endStream, true), streamId));
}

// The server side of an open connection with an AnsweringSink, fed with frames as a client would send them.
// 'nbActiveStreams' streams (ids 1, 3, 5...) are opened first by a request without END_STREAM, and stay open.
class SinkConnectionBench {
 public:
  explicit SinkConnectionBench(uint32_t nbActiveStreams = 0)
      : _connection(Http2Config{}
                        .withMaxConcurrentStreams(std::numeric_limits<uint32_t>::max())
                        .withMaxStreamsPerConnection(std::numeric_limits<uint32_t>::max()),
                    true),
        _sink(_connection),
        _nbActiveStreams(nbActiveStreams) {
    RawBytes setup;
    setup.append(reinterpret_cast<const std::byte*>(kConnectionPreface.data()), kConnectionPreface.size());
    WriteSettingsFrame(setup, {});
    WriteSettingsAckFrame(setup);
    feed(setup);

    RawBytes input;
    for (uint32_t idx = 0; idx < nbActiveStreams; ++idx) {
      const RawBytes& block = idx == 0 ? _blocks.first : _blocks.next;
      const std::size_t framePos = input.size();
      input.ensureAvailableCapacityExponential(FrameHeader::kSize + block.size());
      input.addSize(FrameHeader::kSize);
      input.unchecked_append(block);
      WriteHeadersFrame(input.data() + framePos, block.size(), firstFreeStreamId(), false);
      ++_nbOpenedStreams;
    }
    feed(input);
    checkActiveStreams();
  }

  // Process all of 'input' (whole frames), then write all the output.
  void feed(std::span<const std::byte> input) {
    while (!input.empty()) {
      const auto result = _connection.processInput(input);
      if (result.bytesConsumed == 0 || result.action == Http2Connection::ProcessResult::Action::Error ||
          result.action == Http2Connection::ProcessResult::Action::GoAway) {
        std::fprintf(stderr, "HTTP/2 connection stopped processing (action %d)\n", static_cast<int>(result.action));
        std::abort();
      }
      input = input.subspan(result.bytesConsumed);
    }
    _connection.onOutputWritten(_connection.pendingOutputSize());
  }

  // Abort if a stream opened by the constructor was closed (a window overflow for instance): the benchmark would
  // go on with frames that are ignored, and report the cost of ignoring them.
  void checkActiveStreams() const {
    if (_connection.activeStreamCount() != _nbActiveStreams) {
      std::fprintf(stderr, "HTTP/2 connection has %u active streams instead of %u\n", _connection.activeStreamCount(),
                   _nbActiveStreams);
      std::abort();
    }
  }

  [[nodiscard]] Http2Connection& connection() noexcept { return _connection; }

  [[nodiscard]] const RequestFieldBlocks& blocks() const noexcept { return _blocks; }

  // The lowest stream id above the ones opened by the constructor.
  [[nodiscard]] uint32_t firstFreeStreamId() const noexcept { return 1U + (2U * _nbOpenedStreams); }

 private:
  Http2Connection _connection;
  AnsweringSink _sink;
  RequestFieldBlocks _blocks;
  uint32_t _nbActiveStreams;
  uint32_t _nbOpenedStreams{0};
};

// 'batch' requests per input, each in a new stream (HEADERS with END_STREAM) answered and closed from the callback,
// on a connection with 'nbActiveStreams' other streams that stay open.
void RunAnsweredStreams(benchmark::State& state, uint32_t batch, uint32_t nbActiveStreams) {
  SinkConnectionBench bench(nbActiveStreams);
  const RequestFieldBlocks& blocks = bench.blocks();
  uint32_t streamId = bench.firstFreeStreamId();

  RawBytes input(FrameHeader::kSize + blocks.first.size());
  input.addSize(FrameHeader::kSize);
  input.unchecked_append(blocks.first);
  WriteHeadersFrame(input.data(), blocks.first.size(), streamId, true);
  streamId += 2U;
  bench.feed(input);

  const std::size_t frameSize = FrameHeader::kSize + blocks.next.size();
  input.clear();
  input.reserve(frameSize * batch);
  for (uint32_t idx = 0; idx < batch; ++idx) {
    input.addSize(FrameHeader::kSize);
    input.unchecked_append(blocks.next);
  }

  for ([[maybe_unused]] auto iter : state) {
    for (uint32_t idx = 0; idx < batch; ++idx) {
      WriteHeadersFrame(input.data() + (idx * frameSize), blocks.next.size(), streamId, true);
      streamId += 2U;
    }
    bench.feed(input);
  }
  bench.checkActiveStreams();
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * batch);
}

void BM_ConnectionSinkAnsweredStreams(benchmark::State& state) {
  RunAnsweredStreams(state, static_cast<uint32_t>(state.range(0)), 0U);
}
BENCHMARK(BM_ConnectionSinkAnsweredStreams)->Arg(1)->Arg(16);

// 'batch' DATA frames per input on an open stream, delivered to the sink.
void BM_ConnectionSinkDataFrames(benchmark::State& state) {
  const auto batch = static_cast<uint32_t>(state.range(0));
  constexpr uint32_t kPayloadSize = 128;
  SinkConnectionBench bench(1U);

  RawBytes input;
  for (uint32_t idx = 0; idx < batch; ++idx) {
    std::byte* pData = PrepareDataFrameGetStartPtr(input, 1U, kPayloadSize, false);
    std::memset(pData, 'D', kPayloadSize);
  }

  for ([[maybe_unused]] auto iter : state) {
    bench.feed(input);
  }
  bench.checkActiveStreams();
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * batch);
}
BENCHMARK(BM_ConnectionSinkDataFrames)->Arg(1)->Arg(16);

// A request body received as DATA frames of the given payload size on one stream: the cost per frame, and the
// WINDOW_UPDATE frames (stream and connection) sent back per MiB received.
void BM_ConnectionReceiveBody(benchmark::State& state) {
  const auto payloadSize = static_cast<uint32_t>(state.range(0));
  constexpr uint32_t kBatch = 16;
  SinkConnectionBench bench(1U);
  Http2Connection& connection = bench.connection();

  RawBytes input;
  for (uint32_t idx = 0; idx < kBatch; ++idx) {
    std::byte* pData = PrepareDataFrameGetStartPtr(input, 1U, payloadSize, false);
    std::memset(pData, 'D', payloadSize);
  }

  uint64_t nbWindowUpdates = 0;
  for ([[maybe_unused]] auto iter : state) {
    std::span<const std::byte> remaining(input);
    while (!remaining.empty()) {
      const auto result = connection.processInput(remaining);
      remaining = remaining.subspan(result.bytesConsumed);
    }
    // The output is only made of WINDOW_UPDATE frames, of the same size.
    nbWindowUpdates += connection.pendingOutputSize() / (FrameHeader::kSize + sizeof(uint32_t));
    connection.onOutputWritten(connection.pendingOutputSize());
  }
  bench.checkActiveStreams();
  const auto nbBytes = static_cast<double>(state.iterations()) * kBatch * payloadSize;
  state.counters["window_updates_per_MiB"] = static_cast<double>(nbWindowUpdates) * (1024.0 * 1024.0) / nbBytes;
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kBatch);
  state.SetBytesProcessed(static_cast<int64_t>(nbBytes));
}
BENCHMARK(BM_ConnectionReceiveBody)->Arg(1024)->Arg(16384);

// ---------------------------------------------------------------------------
// Connection with many active streams: randomized lookup, DATA and WINDOW_UPDATE processing, close and prune churn
// ---------------------------------------------------------------------------

// Random ids among the first 'nbStreams' streams of a client (1, 3, 5...), cycled. The sequence is long enough for
// the branch predictor not to learn it, and is the same in all the runs.
class RandomStreamIds {
 public:
  explicit RandomStreamIds(uint32_t nbStreams) : _ids(kNbIds) {
    std::mt19937 gen(20261114U);
    for (uint32_t& id : _ids) {
      id = 1U + (2U * (static_cast<uint32_t>(gen()) % nbStreams));
    }
  }

  [[nodiscard]] uint32_t next() noexcept { return _ids[_pos++ & (kNbIds - 1U)]; }

 private:
  static constexpr uint32_t kNbIds = 1U << 16U;

  vector<uint32_t> _ids;
  uint32_t _pos{0};
};

constexpr uint32_t kStreamFramesBatch = 64;

// Lookup of a random stream among 'active' ones, 'burst' times in a row: a request looks its stream up several times
// (HEADERS received, response HEADERS and DATA sent, closing), frames of distinct streams being interleaved.
void BM_ConnectionStreamLookup(benchmark::State& state) {
  const auto nbActiveStreams = static_cast<uint32_t>(state.range(0));
  const auto burst = static_cast<uint32_t>(state.range(1));
  SinkConnectionBench bench(nbActiveStreams);
  RandomStreamIds ids(nbActiveStreams);
  Http2Connection& connection = bench.connection();

  for ([[maybe_unused]] auto iter : state) {
    uint32_t streamId = ids.next();
    for (uint32_t idx = 0; idx < burst; ++idx) {
      benchmark::DoNotOptimize(streamId);
      Http2Stream* pStream = connection.getStream(streamId);
      benchmark::DoNotOptimize(pStream);
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * burst);
}
BENCHMARK(BM_ConnectionStreamLookup)->ArgsProduct({{1, 100, 1000, 10000}, {1, 8}})->ArgNames({"active", "burst"});

// Batches of DATA frames, each on a random stream among 'active' ones, delivered to the sink. Their stream ids are
// written before each batch (timed: a store per frame).
void BM_ConnectionStreamDataFrames(benchmark::State& state) {
  const auto nbActiveStreams = static_cast<uint32_t>(state.range(0));
  constexpr uint32_t kPayloadSize = 16;
  constexpr std::size_t kFrameSize = FrameHeader::kSize + kPayloadSize;
  SinkConnectionBench bench(nbActiveStreams);
  RandomStreamIds ids(nbActiveStreams);

  RawBytes input;
  for (uint32_t idx = 0; idx < kStreamFramesBatch; ++idx) {
    std::byte* pData = PrepareDataFrameGetStartPtr(input, 1U, kPayloadSize, false);
    std::memset(pData, 'D', kPayloadSize);
  }

  for ([[maybe_unused]] auto iter : state) {
    for (uint32_t idx = 0; idx < kStreamFramesBatch; ++idx) {
      WriteFrameHeader(input.data() + (idx * kFrameSize),
                       FrameHeader(kPayloadSize, FrameType::Data, FrameFlags::None, ids.next()));
    }
    bench.feed(input);
  }
  bench.checkActiveStreams();
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kStreamFramesBatch);
}
BENCHMARK(BM_ConnectionStreamDataFrames)->Arg(1)->Arg(100)->Arg(1000)->Arg(10000)->ArgName("active");

// Batches of WINDOW_UPDATE frames, each on a random stream among 'active' ones.
void BM_ConnectionStreamWindowUpdates(benchmark::State& state) {
  const auto nbActiveStreams = static_cast<uint32_t>(state.range(0));
  constexpr std::size_t kFrameSize = FrameHeader::kSize + sizeof(uint32_t);
  SinkConnectionBench bench(nbActiveStreams);
  RandomStreamIds ids(nbActiveStreams);

  // The smallest increment: a stream accepts 2^31 - 65536 of them before its send window overflows (which closes it,
  // see checkActiveStreams()).
  RawBytes input;
  for (uint32_t idx = 0; idx < kStreamFramesBatch; ++idx) {
    WriteWindowUpdateFrame(input, 1U, 1U);
  }

  for ([[maybe_unused]] auto iter : state) {
    for (uint32_t idx = 0; idx < kStreamFramesBatch; ++idx) {
      WriteFrameHeader(input.data() + (idx * kFrameSize),
                       FrameHeader(sizeof(uint32_t), FrameType::WindowUpdate, FrameFlags::None, ids.next()));
    }
    bench.feed(input);
  }
  bench.checkActiveStreams();
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * kStreamFramesBatch);
}
BENCHMARK(BM_ConnectionStreamWindowUpdates)->Arg(1)->Arg(100)->Arg(1000)->Arg(10000)->ArgName("active");

// Streams opened, answered, closed then pruned (16 requests per input) beside 'active' streams that stay open.
void BM_ConnectionStreamChurn(benchmark::State& state) {
  RunAnsweredStreams(state, 16U, static_cast<uint32_t>(state.range(0)));
}
BENCHMARK(BM_ConnectionStreamChurn)->Arg(1)->Arg(100)->Arg(1000)->Arg(10000)->ArgName("active");

// ---------------------------------------------------------------------------
// Stream table candidates: the flat hash map of Http2Connection, with a cache of the last stream, or a dense table
// ---------------------------------------------------------------------------

// The table of Http2Connection.
class FlatMapStreamTable {
 public:
  [[nodiscard]] Http2Stream* find(uint32_t streamId) noexcept {
    const auto it = _streams.find(streamId);
    return it == _streams.end() ? nullptr : &it->second;
  }

  Http2Stream& open(uint32_t streamId) {
    return _streams.try_emplace(streamId, streamId, kDefaultInitialWindowSize).first->second;
  }

  void erase(uint32_t streamId) { _streams.erase(streamId); }

 private:
  flat_hash_map<uint32_t, Http2Stream> _streams;
};

// The same, remembering the last stream found or opened: the entries move when another one is inserted or erased,
// which forgets it.
class CachedFlatMapStreamTable {
 public:
  [[nodiscard]] Http2Stream* find(uint32_t streamId) noexcept {
    if (streamId == _lastStreamId) {
      return _pLastStream;
    }
    const auto it = _streams.find(streamId);
    if (it == _streams.end()) {
      return nullptr;
    }
    _lastStreamId = streamId;
    _pLastStream = &it->second;
    return _pLastStream;
  }

  Http2Stream& open(uint32_t streamId) {
    _pLastStream = &_streams.try_emplace(streamId, streamId, kDefaultInitialWindowSize).first->second;
    _lastStreamId = streamId;
    return *_pLastStream;
  }

  void erase(uint32_t streamId) {
    _streams.erase(streamId);
    _lastStreamId = 0;
  }

 private:
  flat_hash_map<uint32_t, Http2Stream> _streams;
  Http2Stream* _pLastStream{nullptr};
  uint32_t _lastStreamId{0};  // 0 is not the id of a stream
};

// A dense table: the streams sorted by id in one array. The ids of a peer increase, so a stream is opened at the
// end; erasing one moves the streams opened after it.
class SortedVectorStreamTable {
 public:
  [[nodiscard]] Http2Stream* find(uint32_t streamId) noexcept {
    const auto it = std::ranges::lower_bound(_streams, streamId, {}, &Http2Stream::id);
    return it == _streams.end() || it->id() != streamId ? nullptr : &*it;
  }

  Http2Stream& open(uint32_t streamId) { return _streams.emplace_back(streamId, kDefaultInitialWindowSize); }

  void erase(uint32_t streamId) {
    const auto it = std::ranges::lower_bound(_streams, streamId, {}, &Http2Stream::id);
    if (it != _streams.end() && it->id() == streamId) {
      _streams.erase(it);
    }
  }

 private:
  vector<Http2Stream> _streams;
};

// Lookup of a random stream among 'active' ones, 'burst' times in a row.
template <class StreamTable>
void BM_StreamTableLookup(benchmark::State& state) {
  const auto nbActiveStreams = static_cast<uint32_t>(state.range(0));
  const auto burst = static_cast<uint32_t>(state.range(1));
  StreamTable table;
  for (uint32_t idx = 0; idx < nbActiveStreams; ++idx) {
    table.open(1U + (2U * idx));
  }
  RandomStreamIds ids(nbActiveStreams);

  for ([[maybe_unused]] auto iter : state) {
    uint32_t streamId = ids.next();
    for (uint32_t idx = 0; idx < burst; ++idx) {
      benchmark::DoNotOptimize(streamId);
      Http2Stream* pStream = table.find(streamId);
      benchmark::DoNotOptimize(pStream);
    }
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()) * burst);
}

// The stream table operations of a request answered at once, beside 'active' streams that stay open: its stream is
// opened, looked up 5 times (the response HEADERS and DATA, the window checks, the closing), and the stream closed
// 16 requests earlier is looked up and erased (see Http2Connection::pruneClosedStreams()).
template <class StreamTable>
void BM_StreamTableRequest(benchmark::State& state) {
  const auto nbActiveStreams = static_cast<uint32_t>(state.range(0));
  constexpr uint32_t kNbLookups = 5;
  constexpr uint32_t kNbRetainedClosedStreams = 16;
  StreamTable table;
  uint32_t streamId = 1;
  for (uint32_t idx = 0; idx < nbActiveStreams + kNbRetainedClosedStreams; ++idx) {
    table.open(streamId);
    streamId += 2U;
  }
  uint32_t prunedStreamId = 1U + (2U * nbActiveStreams);

  for ([[maybe_unused]] auto iter : state) {
    Http2Stream* pStream = &table.open(streamId);
    benchmark::DoNotOptimize(pStream);
    for (uint32_t idx = 0; idx < kNbLookups; ++idx) {
      benchmark::DoNotOptimize(streamId);
      pStream = table.find(streamId);
      benchmark::DoNotOptimize(pStream);
    }
    benchmark::DoNotOptimize(prunedStreamId);
    pStream = table.find(prunedStreamId);
    benchmark::DoNotOptimize(pStream);
    table.erase(prunedStreamId);
    streamId += 2U;
    prunedStreamId += 2U;
  }
  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}

BENCHMARK_TEMPLATE(BM_StreamTableLookup, FlatMapStreamTable)
    ->ArgsProduct({{1, 100, 1000, 10000}, {1, 8}})
    ->ArgNames({"active", "burst"});
BENCHMARK_TEMPLATE(BM_StreamTableLookup, CachedFlatMapStreamTable)
    ->ArgsProduct({{1, 100, 1000, 10000}, {1, 8}})
    ->ArgNames({"active", "burst"});
BENCHMARK_TEMPLATE(BM_StreamTableLookup, SortedVectorStreamTable)
    ->ArgsProduct({{1, 100, 1000, 10000}, {1, 8}})
    ->ArgNames({"active", "burst"});

BENCHMARK_TEMPLATE(BM_StreamTableRequest, FlatMapStreamTable)
    ->Arg(1)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->ArgName("active");
BENCHMARK_TEMPLATE(BM_StreamTableRequest, CachedFlatMapStreamTable)
    ->Arg(1)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->ArgName("active");
BENCHMARK_TEMPLATE(BM_StreamTableRequest, SortedVectorStreamTable)
    ->Arg(1)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->ArgName("active");

}  // namespace
}  // namespace aeronet::http2

BENCHMARK_MAIN();
