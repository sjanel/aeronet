#include <benchmark/benchmark.h>

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

#include "aeronet/decimal-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/lower-ascii-key.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/ndigits.hpp"

namespace aeronet {
namespace {

HttpResponse BuildResponse(std::size_t headerCount, bool addTarget) {
  HttpResponse response;
  for (std::size_t index = 0; index < headerCount; ++index) {
    const std::string headerName = "x-benchmark-header-" + std::to_string(index);
    response.headerAddLine(LowerAsciiKey{headerName}, "representative-header-value-" + std::to_string(index));
  }
  if (addTarget) {
    response.headerAddLine("content-language", "en-US");
  }
  return response;
}

void BM_HeaderValuePresentAtEnd(benchmark::State& state) {
  const auto response = BuildResponse(static_cast<std::size_t>(state.range(0)), true);

  for (auto _ : state) {
    benchmark::DoNotOptimize(response.headerValue("content-language"));
  }

  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(response.headersFlatView().size()));
}

void BM_HeaderValueAbsent(benchmark::State& state) {
  const auto response = BuildResponse(static_cast<std::size_t>(state.range(0)), false);

  for (auto _ : state) {
    benchmark::DoNotOptimize(response.headerValue("content-language"));
  }

  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(response.headersFlatView().size()));
}

// Headers a handler typically adds, of various lengths, some of the length of a reserved header (checked on each add).
constexpr std::string_view kHandlerHeaders[][2]{
    {"cache-control", "no-cache"},
    {"x-request-id", "0123456789abcdef"},
    {"etag", "\"33a64df551425fcc55e4d42a148795d9f25f89d4\""},
    {"vary", "origin"},
    {"set-cookie", "session=38afes7a8; Path=/; Secure; HttpOnly"},
    {"access-control-allow-origin", "https://example.com"},
    {"last-modified", "Wed, 21 Oct 2015 07:28:00 GMT"},
    {"x-content-type-options", "nosniff"},
};

// Cost of headerAddLine() on a response without body, the room for the headers being reserved up front.
void BM_HeaderAddLine(benchmark::State& state) {
  for (auto _ : state) {
    HttpResponse response(1024, http::StatusCodeOK);
    for (const auto& [name, value] : kHandlerHeaders) {
      response.headerAddLine(LowerAsciiKey{name}, value);
    }
    benchmark::DoNotOptimize(response);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(std::size(kHandlerHeaders)));
}

// Same, through header(), which first looks the header up.
void BM_HeaderSet(benchmark::State& state) {
  for (auto _ : state) {
    HttpResponse response(1024, http::StatusCodeOK);
    for (const auto& [name, value] : kHandlerHeaders) {
      response.header(LowerAsciiKey{name}, value);
    }
    benchmark::DoNotOptimize(response);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(std::size(kHandlerHeaders)));
}

// Names of the headers whose value is formatted below, as a Content-Range value.
constexpr std::string_view kFormattedHeaders[]{
    "content-range", "x-range-1", "x-range-2", "x-range-3", "x-range-4", "x-range-5", "x-range-6", "x-range-7",
};

constexpr std::size_t kMaxRangeLen = std::string_view("bytes ").size() + (3U * 20U) + 2U;

char* WriteRange(std::uint64_t first, std::uint64_t length, std::uint64_t total, char* out) {
  out = Append(std::string_view("bytes "), out);
  out = WriteUInt(out, first, ndigits(first));
  *out++ = '-';
  const std::uint64_t last = first + length - 1U;
  out = WriteUInt(out, last, ndigits(last));
  *out++ = '/';
  return WriteUInt(out, total, ndigits(total));
}

// A value formatted by the caller into a stack buffer, then copied by headerAddLine().
void BM_HeaderAddLineFormatted(benchmark::State& state) {
  std::uint64_t first = 0;
  for (auto _ : state) {
    HttpResponse response(1024, http::StatusCodeOK);
    for (const std::string_view name : kFormattedHeaders) {
      char buf[kMaxRangeLen];
      const char* end = WriteRange(first, 4096U, std::uint64_t{1} << 40U, buf);
      response.headerAddLine(LowerAsciiKey{name}, std::string_view(buf, end));
      first += 4096U;
    }
    benchmark::DoNotOptimize(response);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(std::size(kFormattedHeaders)));
}

// Same value, written in place by headerInlineAddLine().
void BM_HeaderInlineAddLineFormatted(benchmark::State& state) {
  std::uint64_t first = 0;
  for (auto _ : state) {
    HttpResponse response(1024, http::StatusCodeOK);
    for (const std::string_view name : kFormattedHeaders) {
      response.headerInlineAddLine(LowerAsciiKey{name}, kMaxRangeLen, [first](char* out) -> std::size_t {
        return static_cast<std::size_t>(WriteRange(first, 4096U, std::uint64_t{1} << 40U, out) - out);
      });
      first += 4096U;
    }
    benchmark::DoNotOptimize(response);
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(std::size(kFormattedHeaders)));
}

BENCHMARK(BM_HeaderValuePresentAtEnd)->Arg(10)->Arg(35)->Arg(100);
BENCHMARK(BM_HeaderValueAbsent)->Arg(10)->Arg(35)->Arg(100);
BENCHMARK(BM_HeaderAddLine);
BENCHMARK(BM_HeaderSet);
BENCHMARK(BM_HeaderAddLineFormatted);
BENCHMARK(BM_HeaderInlineAddLineFormatted);

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
