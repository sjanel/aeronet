// Cost of the JavaScript runtime of a WebClient page, without HTTP:
//   the setup of a page (QuickJS-ng runtime and context, parsing, DOM and window bindings, prelude), with the engine
//   alone, for the first page of a WebClient, which compiles the prelude, and with the first use of the lazy parts of
//   the prelude (network APIs, observers and media queries, Intl);
//   a call of a native DOM binding from a script (a getter returning a cached node wrapper), against the same loop
//   without the call; the lookup of a global that does not exist; the insertion of a tree while a script element
//   created by a script did not start;
//   the bindings that convert or build strings (storage, tagName, URL reflection, location and history, character
//   data), the overhead of WebPage::evaluate(), timers run by the page loop, and document.write();
//   a large inline script compiled at each load, against the same script read back from the script cache.
#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
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

// The setup of a page whose scripts use the network and text APIs (fetch(), URL...): their part of the prelude is read
// and run on the first use of one of them.
void BM_PageSetupWithNetworkApi(benchmark::State& state) {
  internal::WebSession session{WebClientConfig{}};
  for (auto _ : state) {
    internal::Page page(session, "https://example.com/", http::StatusCodeOK, kSmallPage, "text/html");
    auto result = page.evaluate("typeof fetch", std::chrono::seconds{10});
    benchmark::DoNotOptimize(result);
  }
}
BENCHMARK(BM_PageSetupWithNetworkApi);

// The same with the first use of the observers and media queries (matchMedia()), then of Intl.
void RunPageSetupWith(benchmark::State& state, std::string_view script) {
  internal::WebSession session{WebClientConfig{}};
  for (auto _ : state) {
    internal::Page page(session, "https://example.com/", http::StatusCodeOK, kSmallPage, "text/html");
    auto result = page.evaluate(script, std::chrono::seconds{10});
    benchmark::DoNotOptimize(result);
  }
}

void BM_PageSetupWithViewApi(benchmark::State& state) {
  RunPageSetupWith(state, "matchMedia('(min-width: 768px)').matches");
}
BENCHMARK(BM_PageSetupWithViewApi);

void BM_PageSetupWithIntl(benchmark::State& state) {
  RunPageSetupWith(state, "new Intl.NumberFormat('en-US', {style: 'currency', currency: 'USD'}).format(1234.5)");
}
BENCHMARK(BM_PageSetupWithIntl);

// A script replacing a global of the network part (a polyfill, an instrumented fetch()) before any use: the part is
// not set up, against BM_PageSetup and BM_PageSetupWithNetworkApi.
void BM_PageSetupReplacingFetch(benchmark::State& state) { RunPageSetupWith(state, "window.fetch = null"); }
BENCHMARK(BM_PageSetupReplacingFetch);

constexpr int kLoopIterations = 100000;

// The time of one iteration of a script loop, whose body is 'statement' ('n' is a number, 'body' the body element, 'a'
// a detached link with a relative href, 'text' the text node of the price), after 'setup' (run once by each script).
void RunLoop(benchmark::State& state, std::string_view statement, std::string_view setup = {}) {
  internal::WebSession session{WebClientConfig{}};
  internal::Page page(session, "https://example.com/", http::StatusCodeOK, kSmallPage, "text/html");
  const std::string script =
      "{ let n = 0; const body = document.body; const a = document.createElement('a'); a.href = '/path/to?q=1'; "
      "const text = document.querySelector('.price').firstChild; " +
      std::string(setup) + "; for (let i = 0; i < " + std::to_string(kLoopIterations) + "; ++i) { " +
      std::string(statement) + "; } n }";
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

// The comparison of the sorts: two ASCII strings compared natively, in the order of the Intl part.
void BM_LocaleCompare(benchmark::State& state) { RunLoop(state, "n += 'Abc'.localeCompare((i & 1) ? 'abd' : 'ABC')"); }
BENCHMARK(BM_LocaleCompare);

// A move of a node: the insertion checks for the scripts that scripts created (none here).
void BM_AppendChild(benchmark::State& state) { RunLoop(state, "body.appendChild(a)"); }
BENCHMARK(BM_AppendChild);

// A move of a tree of 100 elements, then the same with a script element created and never inserted (feature
// detection), out of the tree: the insertion checks that created script, not each node of the tree.
constexpr std::string_view kTree =
    "const tree = document.createElement('ul'); for (let i = 0; i < 100; ++i) tree.appendChild(document.createElement("
    "'li'))";

void BM_AppendTree(benchmark::State& state) { RunLoop(state, "body.appendChild(tree)", kTree); }
BENCHMARK(BM_AppendTree);

void BM_AppendTreeWithACreatedScript(benchmark::State& state) {
  RunLoop(state, "body.appendChild(tree)", std::string("const probe = document.createElement('script'); ") += kTree);
}
BENCHMARK(BM_AppendTreeWithACreatedScript);

// The lookup of a global that does not exist (feature detection), through the lazy globals of the window, and the same
// for a name that looks like an HTML element interface.
void BM_MissingGlobal(benchmark::State& state) { RunLoop(state, "n += typeof requestIdleCallbackX === 'undefined'"); }
BENCHMARK(BM_MissingGlobal);

void BM_MissingHtmlInterfaceName(benchmark::State& state) {
  RunLoop(state, "n += typeof HTMLMarqueeElement === 'undefined'");
}
BENCHMARK(BM_MissingHtmlInterfaceName);

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
    internal::Page::Outcome outcome = page.run(nullptr, LoadState::NetworkIdle, SteadyClock::time_point::max());
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

// An inline script created and inserted by a script: it runs during its insertion.
void BM_InsertedInlineScript(benchmark::State& state) {
  RunPage(state,
          "const s = document.createElement('script'); s.text = 'window.n = ' + (i & 7); document.body.append(s)");
}
BENCHMARK(BM_InsertedInlineScript);

// A page of a large inline script (2000 functions, about 250 KB) loaded with its setup, the script compiled at each
// load without the script cache of the client, read back from it with.
void RunLargeScript(benchmark::State& state, std::size_t maxScriptCacheBytes) {
  WebClientConfig config;
  config.maxScriptCacheBytes = maxScriptCacheBytes;
  internal::WebSession session{config};
  std::string html(kSmallPage);
  html.append("<script>");
  for (int fn = 0; fn < 2000; ++fn) {
    html.append("function f")
        .append(std::to_string(fn))
        .append("(a, b) { const c = a * ")
        .append(std::to_string(fn))
        .append(" + b; return c > 10 ? [c, a, b].join('-') : { a, b, c, d: `${a}:${b}` }; }\n");
  }
  html.append("window.done = typeof f1999;</script>");
  for (auto _ : state) {
    internal::Page page(session, "https://example.com/", http::StatusCodeOK, html, "text/html");
    internal::Page::Outcome outcome = page.run(nullptr, LoadState::Load, SteadyClock::time_point::max());
    benchmark::DoNotOptimize(outcome);
  }
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(html.size()));
}

void BM_LargeScriptCompiled(benchmark::State& state) { RunLargeScript(state, 0); }
BENCHMARK(BM_LargeScriptCompiled);

void BM_LargeScriptFromCache(benchmark::State& state) { RunLargeScript(state, std::size_t{64} << 20U); }
BENCHMARK(BM_LargeScriptFromCache);

}  // namespace

}  // namespace aeronet

BENCHMARK_MAIN();
