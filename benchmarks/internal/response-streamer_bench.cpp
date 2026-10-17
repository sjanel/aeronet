// Micro-benchmark for the HTTP client ResponseStreamer: a content-coded response body delivered to a ResponseBodySink
// as it arrives, decoded piece by piece. Each received piece (one read, or one HTTP/2 DATA frame) goes through the
// decoder, then the decoded bytes go to the sink. Measures the decoding and delivery overhead per piece size.
#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "aeronet/compression-config.hpp"
#include "aeronet/compression-test-helpers.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-client-codec.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/internal/response-streamer.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/response-body-sink.hpp"

namespace aeronet {
namespace {

// Counts the delivered bytes.
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

// Streams a 4 MiB body (half random, half patterned) coded with the encoding of index state.range(0) in
// test::SupportedEncodings(), received in pieces of state.range(1) bytes.
void BM_StreamDecodedBody(benchmark::State& state) {
  const auto encodings = test::SupportedEncodings();
  if (static_cast<std::size_t>(state.range(0)) >= encodings.size()) {
    state.SkipWithMessage("encoding not compiled in");
    return;
  }
  const Encoding encoding = encodings[static_cast<decltype(encodings)::size_type>(state.range(0))];
  const auto pieceSize = static_cast<std::size_t>(state.range(1));
  const RawChars payload = test::MakeMixedPayload(2UL << 20U, 2UL << 20U);
  const RawChars encoded = test::Compress(encoding, std::string_view(payload));
  const std::string_view body(encoded);

  HttpClientConfig config;
  internal::HttpClientCodec codec(CompressionConfig{});
  internal::ResponseStreamer streamer;
  CountingSink sink;
  for ([[maybe_unused]] auto _ : state) {
    streamer.beginRequest(&sink, config, codec);
    streamer.beginExchange(false, false);
    HttpResponse head(http::StatusCodeOK);
    head.headerAddLine(http::ContentEncoding, GetEncodingStr(encoding));
    bool stream = false;
    if (streamer.startResponse(head, {.contentType = {}, .size = body.size()}, stream) != HttpClientErrc::noError ||
        !stream) {
      state.SkipWithError("response not streamed");
      return;
    }
    for (std::size_t off = 0; off < body.size(); off += pieceSize) {
      if (!streamer.feed(body.substr(off, pieceSize))) {
        state.SkipWithError("decoding failed");
        return;
      }
    }
    if (!streamer.finish()) {
      state.SkipWithError("incomplete coded body");
      return;
    }
  }
  if (sink.bytes != static_cast<std::size_t>(state.iterations()) * payload.size()) {
    state.SkipWithError("decoded body size mismatch");
  }
  state.SetLabel(std::string(GetEncodingStr(encoding)));
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(payload.size()));
}

// Encodings: gzip, deflate, br, zstd (as compiled in). Pieces: a small read, the default read size (also the default
// HTTP/2 frame size), a large read.
BENCHMARK(BM_StreamDecodedBody)
    ->ArgsProduct({{0, 1, 2, 3}, {1024, 16384, 65536}})
    ->ArgNames({"encoding", "piece"})
    ->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
