// HTTP/2 request handling micro-benchmarks: the server side of a stream, from its HEADERS frame to its response.
// Measures the HPACK decoding of the request head, the building of its HttpRequestView, the routing and the response
// encoding, as done for each stream of a connection.
#include <benchmark/benchmark.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

#include "aeronet/connection-state.hpp"
#include "aeronet/hpack.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/http2-frame-types.hpp"
#include "aeronet/http2-frame.hpp"
#include "aeronet/http2-protocol-handler.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/router.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::http2 {
namespace {

// Fields of a browser-like GET request: the pseudo-header fields, then the regular ones (the first N of them are sent).
constexpr std::array<http::HeaderView, 4> kPseudoHeaderFields{{
    {":method", "GET"},
    {":scheme", "https"},
    {":authority", "bench.example.com"},
    {":path", "/api/v1/items?page=2&sort=name"},
}};

constexpr std::array<http::HeaderView, 24> kRegularFields{{
    {"user-agent", "Mozilla/5.0 (X11; Linux x86_64; rv:131.0) Gecko/20100101 Firefox/131.0"},
    {"accept", "application/json, text/plain, */*"},
    {"accept-language", "en-US,en;q=0.5"},
    {"accept-encoding", "gzip, deflate, br, zstd"},
    {"referer", "https://bench.example.com/dashboard"},
    {"cookie", "session=7f3a9c2e5b1d4f6a8e0c; theme=dark; lang=en"},
    {"sec-fetch-dest", "empty"},
    {"sec-fetch-mode", "cors"},
    {"sec-fetch-site", "same-origin"},
    {"priority", "u=4"},
    {"te", "trailers"},
    {"x-request-id", "4b2c8e1a-9f3d-4c7b-a6e5-1d0f2b3c4a5e"},
    {"x-client-version", "3.14.2"},
    {"authorization", "Bearer eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJiZW5jaCJ9.c2lnbmF0dXJl"},
    {"cache-control", "no-cache"},
    {"pragma", "no-cache"},
    {"origin", "https://bench.example.com"},
    {"dnt", "1"},
    {"x-forwarded-for", "203.0.113.42"},
    {"x-forwarded-proto", "https"},
    {"x-correlation-id", "c0ffee00-1234-5678-9abc-def012345678"},
    {"x-tenant", "acme"},
    {"x-feature-flags", "new-ui,fast-path"},
    {"if-none-match", "W/\"5e8b-1a2b3c4d\""},
}};

// The server side of an HTTP/2 connection, fed with HEADERS frames as a client would send them.
class Http2RequestBench {
 public:
  explicit Http2RequestBench(std::size_t nbRegularFields)
      : _ctx(HttpServerConfig{}, Router{}), _handler(_ctx, _connectionState, kInvalidHandle) {
    _ctx.router.setDefault([](const HttpRequestView&) { return HttpResponse(http::StatusCodeOK, "ok"); });
    _ctx.dateHeader.refreshIfNeeded(std::chrono::steady_clock::now());

    // Connection preface and SETTINGS exchange.
    RawBytes setup;
    setup.append(std::as_bytes(std::span<const char>(kConnectionPreface)));
    WriteSettingsFrame(setup, {});
    feed(setup);
    setup.clear();
    WriteSettingsAckFrame(setup);
    // The largest connection window: the responses never wait for flow-control credit.
    WriteWindowUpdateFrame(setup, 0, kMaxWindowSize - kDefaultInitialWindowSize);
    feed(setup);

    // A client indexes the fields of its first request, and refers to them for the next ones: the first block inserts
    // them in the dynamic table, the second one, sent for each benchmarked stream, references them.
    HpackEncoder encoder(_ctx.config.http2.headerTableSize);
    RawBytes firstBlock;
    RawBytes block;
    for (RawBytes* pBlock : {&firstBlock, &block}) {
      for (const http::HeaderView& field : kPseudoHeaderFields) {
        encoder.encode(*pBlock, field.name, field.value);
      }
      for (std::size_t idx = 0; idx < nbRegularFields; ++idx) {
        encoder.encode(*pBlock, kRegularFields[idx].name, kRegularFields[idx].value);
      }
    }

    _frame = makeHeadersFrame(firstBlock);
    sendRequest();
    _frame = makeHeadersFrame(block);
  }

  // Send the request in a new stream and consume its response.
  void sendRequest() {
    WriteFrameHeader(_frame.data(), FrameHeader(static_cast<uint32_t>(_frame.size() - FrameHeader::kSize),
                                                FrameType::Headers, ComputeHeaderFrameFlags(true, true), _streamId));
    _streamId += 2;
    feed(_frame);
  }

 private:
  [[nodiscard]] static RawBytes makeHeadersFrame(const RawBytes& block) {
    RawBytes frame(FrameHeader::kSize + block.size());
    frame.setSize(FrameHeader::kSize);
    frame.unchecked_append(block);
    return frame;
  }

  void feed(std::span<const std::byte> input) {
    while (!input.empty()) {
      const auto res = _handler.processInput(input);
      if (res.bytesConsumed == 0 || res.action == ProtocolProcessResult::Action::Close ||
          res.action == ProtocolProcessResult::Action::CloseImmediate) {
        std::fprintf(stderr, "HTTP/2 connection stopped processing at stream %u (action %d)\n", _streamId,
                     static_cast<int>(res.action));
        std::abort();
      }
      input = input.subspan(res.bytesConsumed);
    }
    // Write the whole output at once, as a socket taking all of it.
    while (_handler.hasPendingOutput()) {
      _handler.getPendingOutputFragments(_fragments);
      std::size_t nbBytes = 0;
      for (const std::string_view fragment : _fragments) {
        nbBytes += fragment.size();
      }
      _handler.onOutputWritten(nbBytes);
    }
  }

  ::aeronet::internal::ServerContext _ctx;
  ::aeronet::ConnectionState _connectionState;
  Http2ProtocolHandler _handler;
  vector<std::string_view> _fragments;
  RawBytes _frame;
  uint32_t _streamId{1};
};

// One GET request per stream, with the given number of regular header fields.
void BM_Http2GetRequest(benchmark::State& state) {
  Http2RequestBench bench(static_cast<std::size_t>(state.range(0)));
  for ([[maybe_unused]] auto iter : state) {
    bench.sendRequest();
  }
  state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Http2GetRequest)->Arg(1)->Arg(12)->Arg(24);

}  // namespace
}  // namespace aeronet::http2

BENCHMARK_MAIN();
