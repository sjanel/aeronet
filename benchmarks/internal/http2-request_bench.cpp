// HTTP/2 request handling micro-benchmarks: the server side of a stream, from its HEADERS frame to its response.
// Measures the HPACK decoding of the request head, the building of its HttpRequestView, the routing and the response
// encoding, as done for each stream of a connection.
#include <benchmark/benchmark.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <span>
#include <string>
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
  explicit Http2RequestBench(std::size_t nbRegularFields, std::size_t responseBodySize = 2)
      : _ctx(MakeConfig(), Router{}),
        _handler(_ctx, _connectionState, kInvalidHandle),
        _responseBody(responseBodySize, 'x') {
    _ctx.config.validate();
    _ctx.router.setDefault([body = std::string_view(_responseBody)](const HttpRequestView&) {
      return HttpResponse(http::StatusCodeOK, body);
    });

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
    if (feed(_frame) < _responseBody.size()) [[unlikely]] {
      // The benchmark would go on, reporting the cost of requests that are only decoded, or refused.
      std::fprintf(stderr, "HTTP/2 request on stream %u was not answered\n", _streamId - 2U);
      std::abort();
    }

    // Give back the connection credit taken by the response bodies, as a client reading them does: once it is used
    // up (after 2 GiB of bodies), the responses wait for it and the next streams are refused.
    _usedConnectionCredit += _responseBody.size();
    if (_usedConnectionCredit >= kConnectionCreditStep) [[unlikely]] {
      _creditFrame.clear();
      WriteWindowUpdateFrame(_creditFrame, 0, static_cast<uint32_t>(_usedConnectionCredit));
      _usedConnectionCredit = 0;
      feed(_creditFrame);
    }
  }

 private:
  [[nodiscard]] static RawBytes makeHeadersFrame(const RawBytes& block) {
    RawBytes frame(FrameHeader::kSize + block.size());
    frame.setSize(FrameHeader::kSize);
    frame.unchecked_append(block);
    return frame;
  }

  // The streams of a connection are not limited: a benchmark opens millions of them, and the requests received after
  // the GOAWAY sent at the limit (Http2Config::maxStreamsPerConnection) are not served.
  [[nodiscard]] static HttpServerConfig MakeConfig() {
    HttpServerConfig config;
    config.http2.withMaxStreamsPerConnection(std::numeric_limits<uint32_t>::max());
    return config;
  }

  // Process 'input', then write the whole output at once, as a socket taking all of it. Returns the output size.
  std::size_t feed(std::span<const std::byte> input) {
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
    std::size_t outputSize = 0;
    while (_handler.hasPendingOutput()) {
      _handler.getPendingOutputFragments(_fragments);
      std::size_t nbBytes = 0;
      for (const std::string_view fragment : _fragments) {
        nbBytes += fragment.size();
      }
      _handler.onOutputWritten(nbBytes);
      outputSize += nbBytes;
    }
    return outputSize;
  }

  // Connection credit granted again at once (a quarter of the largest window).
  static constexpr std::size_t kConnectionCreditStep = 1UL << 29U;

  ::aeronet::internal::ServerContext _ctx;
  ::aeronet::ConnectionState _connectionState;
  Http2ProtocolHandler _handler;
  std::string _responseBody;
  vector<std::string_view> _fragments;
  RawBytes _frame;
  RawBytes _creditFrame;
  std::size_t _usedConnectionCredit{0};
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

// One GET request per stream, answered with a body of the given size: copied beside its frame header in the output
// of the connection when small, queued with its storage otherwise (see Http2Connection::sendData()).
void BM_Http2GetResponseBody(benchmark::State& state) {
  const auto bodySize = static_cast<std::size_t>(state.range(0));
  Http2RequestBench bench(1, bodySize);
  for ([[maybe_unused]] auto iter : state) {
    bench.sendRequest();
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(bodySize));
}
BENCHMARK(BM_Http2GetResponseBody)->Arg(16)->Arg(256)->Arg(1024)->Arg(4096)->Arg(16384);

}  // namespace
}  // namespace aeronet::http2

BENCHMARK_MAIN();
