// Cost of the JavaScript runtime of a WebClient page, without HTTP:
//   the setup of a page (QuickJS-ng runtime and context, parsing, DOM and window bindings, prelude), with the engine
//   alone, and for the first page of a WebClient, which compiles the prelude;
//   a call of a native DOM binding from a script (a getter returning a cached node wrapper), against the same loop
//   without the call;
//   the bindings that convert or build strings (storage, tagName, URL reflection, location and history, character
//   data), the overhead of WebPage::evaluate(), timers run by the page loop, and document.write().
#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

#include "aeronet/http-status-code.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/web-client-config.hpp"
#include "aeronet/web-client.hpp"
#include "js-engine.hpp"
#include "page.hpp"

namespace aeronet {

namespace {

constexpr std::string_view kSmallPage =
    "<!DOCTYPE html><html><head><title>Product</title></head><body><div id=\"app\"><p class=\"price\">42</p>"
    "</div></body></html>";

internal::JsLimits Limits(const WebClientConfig& config) {
  return internal::JsLimits{config.maxScriptMemory, config.maxScriptStackSize};
}

void BM_JsEngine(benchmark::State& state) {
  const WebClientConfig config;
  for (auto _ : state) {
    internal::JsEngine engine(Limits(config));
    benchmark::DoNotOptimize(engine.context());
  }
}
BENCHMARK(BM_JsEngine);

// The first page of a WebClient also compiles the JavaScript part of the Web APIs (later pages read its bytecode).
void BM_FirstPageOfASession(benchmark::State& state) {
  for (auto _ : state) {
    internal::WebSession session{WebClientConfig{}};
    internal::Page page(session, "https://example.com/", http::StatusCodeOK, kSmallPage, "text/html");
    benchmark::DoNotOptimize(page.context());
  }
}
BENCHMARK(BM_FirstPageOfASession);

void BM_PageSetup(benchmark::State& state) {
  internal::WebSession session{WebClientConfig{}};
  for (auto _ : state) {
    internal::Page page(session, "https://example.com/", http::StatusCodeOK, kSmallPage, "text/html");
    benchmark::DoNotOptimize(page.context());
  }
}
BENCHMARK(BM_PageSetup);

constexpr int kLoopIterations = 100000;

// The time of one iteration of a script loop, whose body is 'statement' ('n' is a number, 'body' the body element, 'a'
// a detached link with a relative href, 'text' the text node of the price).
void RunLoop(benchmark::State& state, std::string_view statement) {
  internal::WebSession session{WebClientConfig{}};
  internal::Page page(session, "https://example.com/", http::StatusCodeOK, kSmallPage, "text/html");
  const std::string script =
      "{ let n = 0; const body = document.body; const a = document.createElement('a'); a.href = '/path/to?q=1'; "
      "const text = document.querySelector('.price').firstChild; for (let i = 0; i < " +
      std::to_string(kLoopIterations) + "; ++i) { " + std::string(statement) + "; } n }";
  for (auto _ : state) {
    auto result = page.evaluate(script, std::chrono::seconds{10});
    benchmark::DoNotOptimize(result);
  }
  state.SetItemsProcessed(state.iterations() * kLoopIterations);
}

void BM_LoopBaseline(benchmark::State& state) { RunLoop(state, "n += i & 1"); }
BENCHMARK(BM_LoopBaseline);

void BM_BindingGetter(benchmark::State& state) { RunLoop(state, "n += body.firstChild === null"); }
BENCHMARK(BM_BindingGetter);

void BM_BindingMethod(benchmark::State& state) { RunLoop(state, "n += body.hasAttribute('id')"); }
BENCHMARK(BM_BindingMethod);

void BM_TagName(benchmark::State& state) { RunLoop(state, "n += body.tagName.length"); }
BENCHMARK(BM_TagName);

void BM_StorageSetGet(benchmark::State& state) {
  RunLoop(state, "localStorage.setItem('key' + (i & 63), 'value'); n += localStorage.getItem('key' + (i & 31)).length");
}
BENCHMARK(BM_StorageSetGet);

void BM_ElementHref(benchmark::State& state) { RunLoop(state, "n += a.href.length"); }
BENCHMARK(BM_ElementHref);

void BM_LocationHash(benchmark::State& state) { RunLoop(state, "location.hash = 'h' + (i & 7)"); }
BENCHMARK(BM_LocationHash);

void BM_PushState(benchmark::State& state) { RunLoop(state, "history.pushState(null, '', '/p' + (i & 63))"); }
BENCHMARK(BM_PushState);

void BM_CharacterData(benchmark::State& state) { RunLoop(state, "text.data = 'v' + (i & 7)"); }
BENCHMARK(BM_CharacterData);

// The cost of WebPage::evaluate() around a script that does nothing.
void BM_EvaluateOverhead(benchmark::State& state) {
  internal::WebSession session{WebClientConfig{}};
  internal::Page page(session, "https://example.com/", http::StatusCodeOK, kSmallPage, "text/html");
  for (auto _ : state) {
    auto result = page.evaluate("0", std::chrono::seconds{10});
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_EvaluateOverhead);

constexpr int kPageLoopItems = 10000;

// A page whose inline script runs 'statement' kPageLoopItems times ('i' the index), loaded until no timer is left: its
// setup is included, a small part of the time.
void RunPage(benchmark::State& state, std::string_view statement) {
  internal::WebSession session{WebClientConfig{}};
  const std::string html = std::string(kSmallPage) + "<script>for (let i = 0; i < " + std::to_string(kPageLoopItems) +
                           "; ++i) { " + std::string(statement) + "; }</script>";
  for (auto _ : state) {
    internal::Page page(session, "https://example.com/", http::StatusCodeOK, html, "text/html");
    const internal::Page::Outcome outcome = page.run(nullptr, LoadState::NetworkIdle, SteadyClock::time_point::max());
    benchmark::DoNotOptimize(outcome);
  }
  state.SetItemsProcessed(state.iterations() * kPageLoopItems);
}

void BM_Timers(benchmark::State& state) { RunPage(state, "setTimeout(() => {}, i & 15)"); }
BENCHMARK(BM_Timers);

void BM_TimersWithArguments(benchmark::State& state) { RunPage(state, "setTimeout((x, y) => x + y, i & 15, i, 1)"); }
BENCHMARK(BM_TimersWithArguments);

void BM_DocumentWrite(benchmark::State& state) { RunPage(state, "document.write('a', 'b', i & 7)"); }
BENCHMARK(BM_DocumentWrite);

}  // namespace

}  // namespace aeronet

BENCHMARK_MAIN();
