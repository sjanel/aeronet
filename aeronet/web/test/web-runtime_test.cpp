// The JavaScript runtime of a page: built-ins, limits (memory, stack, time), evaluate() results and errors, uncaught
// errors, configuration.
#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <numeric>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/chrono-lite.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/test_server_fixture.hpp"
#include "aeronet/web-client-config.hpp"
#include "aeronet/web-client.hpp"
#include "aeronet/web-page.hpp"
#include "web-test-util.hpp"

#ifdef AERONET_ENABLE_GLAZE
#include <glaze/glaze.hpp>

#include "aeronet/address-space.hpp"
#include "aeronet/glaze-round-trip.hpp"
#include "aeronet/web-client-config-glaze.hpp"
#endif

namespace aeronet {
namespace {

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
test::TestServer gServer = test::MakePageServer();

WebPage Load(WebClient& client, std::string_view html, const NavigateOptions& options = {}) {
  return test::Navigate(client, test::PageUrl(gServer, html), options);
}

WebPage LoadEmpty(WebClient& client) { return Load(client, "<p>empty</p>"); }

TEST(WebRuntimeTest, StandardBuiltins) {
  WebClient client;
  WebPage page = LoadEmpty(client);
  EXPECT_EQ(test::Eval(page, "[typeof Promise, typeof Map, typeof WeakRef, typeof Proxy, typeof JSON, typeof BigInt]"),
            R"(["function","function","function","function","object","function"])");
  EXPECT_EQ(test::Eval(page, "btoa('hi') + atob('aGk=')"), R"("aGk=hi")");
  EXPECT_EQ(test::Eval(page, "typeof performance.now() + typeof queueMicrotask + typeof DOMException"),
            R"("numberfunctionfunction")");
  EXPECT_EQ(test::Eval(page, "eval('1 + 1') + new Function('return 2')()"), "4");
  EXPECT_EQ(test::Eval(page, "new Date(0).toISOString()"), R"("1970-01-01T00:00:00.000Z")");
  EXPECT_EQ(test::Eval(page, "[window === globalThis, self === window, top === window, parent === window]"),
            "[true,true,true,true]");
}

TEST(WebRuntimeTest, NoSharedMemory) {
  WebClient client;
  WebPage page = LoadEmpty(client);
  EXPECT_EQ(test::Eval(page, "[typeof SharedArrayBuffer, typeof Atomics, 'SharedArrayBuffer' in globalThis]"),
            R"(["undefined","undefined",false])");
  // The typed arrays are kept.
  EXPECT_EQ(test::Eval(page, "new Uint8Array([1, 2]).length"), "2");
}

TEST(WebRuntimeTest, EvaluateReturnsJson) {
  WebClient client;
  WebPage page = LoadEmpty(client);
  EXPECT_EQ(test::Eval(page, "({a: [1, 'b', null], c: true})"), R"({"a":[1,"b",null],"c":true})");
  EXPECT_EQ(test::Eval(page, "'text'"), R"("text")");
  EXPECT_EQ(test::Eval(page, "undefined"), "");
  EXPECT_EQ(test::Eval(page, "(function () {})"), "");
  EXPECT_EQ(test::Eval(page, "var declared = 3"), "");
  EXPECT_EQ(test::Eval(page, "declared"), "3");
}

TEST(WebRuntimeTest, EvaluateFailsWithTheException) {
  WebClient client;
  WebPage page = LoadEmpty(client);
  auto result = page.evaluate("throw new TypeError('boom')");
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().starts_with("TypeError: boom")) << result.error();
  EXPECT_TRUE(result.error().contains("aeronet:evaluate")) << result.error();  // the stack

  result = page.evaluate("1 +");
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().starts_with("SyntaxError")) << result.error();

  // Not an Error: its string conversion, or a placeholder when it throws.
  result = page.evaluate("throw 42");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), "42");
  result = page.evaluate("throw { toString() { throw 1; } }");
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), "uncaught exception");

  // A value without JSON text.
  result = page.evaluate("const cycle = {}; cycle.self = cycle; cycle");
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().starts_with("TypeError")) << result.error();
}

TEST(WebRuntimeTest, EvaluateRunsTheMicrotasks) {
  WebClient client;
  WebPage page = LoadEmpty(client);
  EXPECT_EQ(test::Eval(page, "Promise.resolve().then(() => { window.done = 1; }); queueMicrotask(() => ++done); 0"),
            "0");
  EXPECT_EQ(test::Eval(page, "done"), "2");
}

TEST(WebRuntimeTest, EvaluateIsStoppedByItsTimeout) {
  WebClient client;
  WebPage page = LoadEmpty(client);
  const auto start = std::chrono::steady_clock::now();
  const auto result = page.evaluate("while (true) {}", std::chrono::milliseconds{50});
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{5});
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error(), "The script ran longer than its timeout of 50 ms");
  // The interruption cannot be caught by the script, and the runtime is still usable.
  EXPECT_FALSE(page.evaluate("try { for (;;) {} } catch (e) { 'caught' }", std::chrono::milliseconds{20}));
  EXPECT_EQ(test::Eval(page, "1 + 1"), "2");
  EXPECT_TRUE(page.uncaughtErrors().empty());
}

TEST(WebRuntimeTest, NavigateIsStoppedByItsTimeout) {
  WebClient client;
  const auto start = std::chrono::steady_clock::now();
  WebPage page = Load(client, "<script>window.before = 1; for (;;) {}</script><script>window.after = 1</script>",
                      {.timeout = std::chrono::milliseconds{100}});
  EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{5});
  EXPECT_EQ(page.stopReason(), StopReason::TimedOut);
  EXPECT_EQ(test::Eval(page, "[window.before, window.after]"), "[1,null]");
  // The interruption is the stop reason, not an error of the page.
  EXPECT_TRUE(page.uncaughtErrors().empty());
}

// The error of the timeout stops the script that inserted a script stopped by it, which cannot catch it: whatever the
// insertion that started the inserted script, the loop would run on past the timeout otherwise. Likewise for the error
// of a listener whose conversion (to report it) runs past the timeout.
TEST(WebRuntimeTest, ScriptsInsertingScriptsAreStoppedByTheTimeout) {
  WebClient client;
  for (const std::string_view statement : {
           "document.body.appendChild(script('for (;;) {}'))",
           "try { document.body.append(script('for (;;) {}')) } catch { window.caught = 1 }",
           "document.body.replaceChild(script('for (;;) {}'), document.body.firstChild)",
           "try { document.body.replaceChildren(script('for (;;) {}')) } catch { window.caught = 1 }",
           "document.body.appendChild(script('')).text = 'for (;;) {}'",
           "document.body.appendChild(script('')).insertAdjacentText('beforeend', 'for (;;) {}')",
           "document.body.appendChild(script('')).insertAdjacentHTML('beforeend', 'for (;;) {}')",
           "document.body.appendChild(script('')).innerHTML = 'for (;;) {}'",
           "document.body.appendChild(script('')).appendChild(document.createTextNode('')).data = 'for (;;) {}'",
           "try { document.body.dispatchEvent(new Event('throw')) } catch { window.caught = 1 }",
       }) {
    const auto start = std::chrono::steady_clock::now();
    WebPage page = Load(client,
                        "<p></p><script>const script = text => Object.assign(document.createElement('script'), {text});"
                        "document.body.addEventListener('throw', () => { throw { toString() { for (;;) {} } } });"
                        "for (;;) { " +
                            std::string(statement) + " }</script><script>window.after = 1</script>",
                        {.timeout = std::chrono::milliseconds{100}});
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds{5}) << statement;
    EXPECT_EQ(page.stopReason(), StopReason::TimedOut) << statement;
    EXPECT_EQ(test::Eval(page, "[window.caught, window.after]"), "[null,null]") << statement;
    // Also in evaluate().
    const auto result = page.evaluate("for (;;) { " + std::string(statement) + " }", std::chrono::milliseconds{50});
    ASSERT_FALSE(result.has_value()) << statement;
    EXPECT_EQ(result.error(), "The script ran longer than its timeout of 50 ms") << statement;
  }
}

TEST(WebRuntimeTest, EndlessMicrotasksAreStoppedByTheTimeout) {
  WebClient client;
  WebPage page = Load(client, "<script>(function loop() { Promise.resolve().then(loop); })()</script>",
                      {.timeout = std::chrono::milliseconds{100}});
  EXPECT_EQ(page.stopReason(), StopReason::TimedOut);
}

TEST(WebRuntimeTest, MemoryLimit) {
  WebClient client(WebClientConfig{.maxScriptMemory = std::size_t{16} << 20U});
  WebPage page = Load(client,
                      "<script>const chunks = []; for (let i = 0; i < 1000; ++i) chunks.push(new Array(1 << 20)."
                      "fill(i));</script><script>window.next = 1</script>");
  ASSERT_EQ(page.uncaughtErrors().size(), 1U);
  EXPECT_TRUE(page.uncaughtErrors()[0].contains("out of memory")) << page.uncaughtErrors()[0];
  // The next scripts run, once the memory of the failed one is released.
  EXPECT_EQ(test::Eval(page, "next"), "1");
}

// Each step of the setup of a page fails in turn as the memory limit grows: no step leaks (QuickJS asserts that no
// object is left when a runtime is freed), and the page loads once the limit is enough.
TEST(WebRuntimeTest, MemoryLimitTooLowForThePageSetup) {
  bool loaded = false;
  int failures = 0;
  for (std::size_t limit = 1024; !loaded; limit += limit / 16) {
    ASSERT_LT(limit, std::size_t{64} << 20U);
    WebClient client(WebClientConfig{.maxScriptMemory = limit});
    try {
      auto page = client.navigate(test::PageUrl(gServer, "<p>x</p>"));
      ASSERT_TRUE(page);
      loaded = true;
    } catch (const std::bad_alloc&) {
      ++failures;
    } catch (const std::runtime_error& ex) {
      EXPECT_TRUE(std::string_view(ex.what()).starts_with("Cannot set up the JavaScript runtime of a page"));
      ++failures;
    }
  }
  EXPECT_GT(failures, 10);
}

// At the end of the stack, the first use of a lazy part of the prelude fails with the RangeError of a stack overflow,
// and works at a smaller depth: a failed setup leaves nothing behind.
TEST(WebRuntimeTest, LazyPartsAtTheEndOfTheStack) {
  WebClient client;
  // The first page compiles the parts, without limits.
  WebPage first = LoadEmpty(client);
  EXPECT_EQ(test::Eval(first, "[typeof URL, typeof matchMedia, typeof Intl]"), R"(["function","function","object"])");
  for (const std::string_view probe : {
           "document.body.getBoundingClientRect().width",
           "typeof matchMedia",
           "(1234.5).toLocaleString()",
           "'\u00e9'.localeCompare('f')",
           "navigator.sendBeacon('/beacon')",
           "new Date(0).toLocaleString()",
           "new Date(0).toLocaleDateString()",
           "new Date(0).toLocaleTimeString()",
       }) {
    WebPage page = LoadEmpty(client);
    const std::string script = "const probe = () => " + std::string(probe) +
                               "; let failures = 0;"
                               "const recurse = n => { try { return recurse(n + 1); } catch (e) {"
                               "try { return probe(); } catch (inner) { ++failures; throw inner; } } };"
                               "[recurse(0), failures > 0]";
    const std::string result = test::Eval(page, script);
    EXPECT_TRUE(result.ends_with(",true]")) << probe << ": " << result;
    // The part is set up for the next uses.
    EXPECT_FALSE(test::Eval(page, probe).empty()) << probe;
  }
}

constexpr std::string_view kRecursionOfDepth3 = "(function depth(n) { return n === 0 ? 0 : 1 + depth(n - 1); })(3)";

// An endless recursion throws a RangeError, after which the page runs its scripts again.
void ExpectEndlessRecursionStopped(WebPage& page) {
  const auto result = page.evaluate("(function recurse(n) { return recurse(n + 1) + 1; })(0)");
  ASSERT_FALSE(result);
  EXPECT_TRUE(result.error().contains("stack")) << result.error();
  EXPECT_EQ(test::Eval(page, kRecursionOfDepth3), "3");
}

// The stack of a JavaScript call depends on the build (QuickJS-ng's interpreter frame: about 9 KiB unoptimized, 70 KiB
// with the sanitizers): the small limit is doubled from one too low for the page setup, which then fails the
// navigation, until a page runs a recursion of depth 3.
TEST(WebRuntimeTest, StackLimit) {
  int setupFailures = 0;
  for (std::size_t stackSize = 1024;; stackSize *= 2) {
    ASSERT_LE(stackSize, WebClientConfig{}.maxScriptStackSize);
    WebClient client(WebClientConfig{.maxScriptStackSize = stackSize});
    try {
      WebPage page = LoadEmpty(client);
      if (page.evaluate(kRecursionOfDepth3)) {
        ExpectEndlessRecursionStopped(page);
        break;
      }
    } catch (const std::runtime_error& ex) {
      EXPECT_TRUE(std::string_view(ex.what()).contains("Maximum call stack size exceeded")) << ex.what();
      ++setupFailures;
    }
  }
  EXPECT_GT(setupFailures, 0);

  WebClient client;
  WebPage page = LoadEmpty(client);
  ExpectEndlessRecursionStopped(page);
}

constexpr std::size_t kSmallMemory = std::size_t{4} << 20U;

// Whether 'script' fails with an out of memory error (uncatchable in QuickJS-ng 0.18, as all its internal errors). The
// memory left for the error depends on the exact allocations of the build: when its message does not fit, QuickJS-ng
// throws an InternalError without one (the text is its name, then its stack), and when even the error object does not
// fit, it throws null instead (to avoid recursing).
testing::AssertionResult RunsOutOfMemory(WebPage& page, std::string_view script) {
  const auto result = page.evaluate(script);
  if (result) {
    return testing::AssertionFailure() << "the script returned " << *result;
  }
  const std::string_view error = result.error();
  const bool withoutMessage = error == "InternalError" || error.starts_with("InternalError\n");
  if (!error.starts_with("InternalError: out of memory") && !withoutMessage && error != "null") {
    return testing::AssertionFailure() << "the script failed with " << error;
  }
  return testing::AssertionSuccess();
}

// The native memory that the scripts make a page hold is bounded like their heap: a loop that adds to it stops with an
// out of memory error, whatever the binding.
TEST(WebRuntimeTest, NativeMemoryOfTheDomIsBounded) {
  WebClient client(WebClientConfig{.maxScriptMemory = kSmallMemory});
  for (const std::string_view grow : {
           "document.body.insertAdjacentHTML('beforeend', '<b></b>')",
           "document.body.innerHTML = '<i title=t>x</i><template><p>in a template</p></template>'",
           "document.body.textContent = 'x'",
           "document.body.insertAdjacentText('beforeend', 'x')",
           "document.createElement('p')",
           "document.createComment('x')",
           "document.createTextNode('').data = big",
           "document.body.setAttribute('a' + i, big)",
           "document.createElementNS('http://www.w3.org/2000/svg', 'svg')",
           "document.createDocumentFragment()",
           "document.body.cloneNode()",
           // A copy of a growing tree: the copies are counted, with their descendants.
           "document.body.appendChild(document.body.cloneNode(true))",
           "document.createDocumentFragment().cloneNode()",
           "document.firstChild.cloneNode()",
       }) {
    WebPage page = LoadEmpty(client);
    EXPECT_TRUE(RunsOutOfMemory(page, "const big = 'x'.repeat(1 << 16); for (let i = 0;; ++i) " + std::string(grow)))
        << grow;
  }
}

// The arrays returned by the bindings are bounded by the heap: a script keeping them runs out of memory in the middle
// of one, which is freed (QuickJS asserts that no value leaks when the runtime is freed), and the next scripts run.
// They are kept by a local of a function, freed when the error leaves it: kept by a global, they would leave the next
// script too little memory to compile, depending on where the limit was reached.
TEST(WebRuntimeTest, ArraysOfTheBindingsRunOutOfMemory) {
  WebClient client(WebClientConfig{.maxScriptMemory = kSmallMemory});
  for (const std::string_view array : {
           "document.querySelectorAll('i')",
           "document.body.childNodes",
           "document.body.children",
           "document.getElementsByTagName('i')",
           "document.getElementsByClassName('c')",
           "document.body.firstChild.getAttributeNames()",
       }) {
    WebPage page = LoadEmpty(client);
    EXPECT_EQ(test::Eval(page,
                         "document.body.innerHTML = '<i class=c></i>'.repeat(4096); const first = "
                         "document.body.firstChild; for (let i = 0; i < 2048; ++i) first.setAttribute('a' + i, ''); 0"),
              "0");
    // Kept in a list of small links, so that the arrays of the bindings take the memory.
    EXPECT_TRUE(
        RunsOutOfMemory(page, "(() => { let kept = null; for (;;) kept = [kept, " + std::string(array) + "] })()"))
        << array;
    EXPECT_EQ(test::Eval(page, "document.querySelectorAll('i').length"), "4096") << array;
  }
}

#if AERONET_WANT_MALLOC_OVERRIDES
// Each allocation of a binding that returns an array fails in turn, mallocs then reallocs: the creation of the array,
// the wrappers of its new nodes and their strings, the growth of its elements, the list of the wrappers. Each failure
// frees what it took (QuickJS asserts that no value leaks when the runtime is freed), until the call succeeds.
TEST(WebRuntimeTest, ArrayBindingsOnEachAllocationFailure) {
  WebClient client;
  WebPage page = LoadEmpty(client);
  for (const std::string_view array : {
           "document.querySelectorAll('i')",
           "document.body.childNodes",
           "document.body.children",
           "document.getElementsByTagName('i')",
           "document.getElementsByClassName('c')",
           "document.body.firstChild.getAttributeNames()",
       }) {
    const std::string script = std::string(array) + ".length";
    // QuickJS-ng creates a native method on its first use, and drops it for good when that creation fails (see the
    // ROADMAP): used once before, as it is not what this test checks.
    ASSERT_FALSE(test::Eval(page, script).empty()) << array;
    for (const bool realloc : {false, true}) {
      for (int successfulAllocs = 0;; ++successfulAllocs) {
        ASSERT_LT(successfulAllocs, 10000) << array;
        // New nodes, without wrappers yet.
        ASSERT_EQ(test::Eval(page, "document.body.innerHTML = '<i class=c a=1 b=2></i><i class=c></i>'.repeat(20); 0"),
                  "0");
        if (realloc) {
          test::FailNextRealloc(successfulAllocs, 1);
        } else {
          test::FailNextMalloc(successfulAllocs, 1);
        }
        std::expected<std::string, std::string> result;
        try {
          result = page.evaluate(script);
        } catch (const std::bad_alloc&) {
          result = std::unexpected("std::bad_alloc");  // of the C++ code of evaluate()
        }
        test::ResetToSysMalloc();
        test::ResetToSysRealloc();
        if (result) {
          EXPECT_EQ(*result, array.ends_with("getAttributeNames()") ? "3" : "40") << array;
          break;
        }
      }
    }
  }
}

// The setup of an HTML element interface (with its parent for the audio one) that runs out of memory leaves nothing
// behind: once memory is available, the next lookup sets it up, whatever allocation failed. The heap is filled to just
// below its limit by a list of small objects before the first lookup, from more of them (no memory left for the lookup)
// to fewer (the whole setup fits).
TEST(WebRuntimeTest, HtmlInterfacesOutOfMemory) {
  WebClient client(WebClientConfig{.maxScriptMemory = kSmallMemory});
  const std::string setup = "var div = document.createElement('div'); var audio = document.createElement('audio'); 0";
  // Each page is set up the same way: the first one compiles the prelude.
  const auto newPage = [&] {
    WebPage page = LoadEmpty(client);
    EXPECT_EQ(test::Eval(page, setup), "0");
    return page;
  };
  static_cast<void>(newPage());
  // The objects, freed when the script returns or fails.
  const auto fillThen = [](int count, std::string_view expression) {
    return "(() => { let head = null; for (let i = 0; i < " + std::to_string(count) +
           "; ++i) head = {next: head}; return " + std::string(expression) + "; })()";
  };
  // The largest number of objects that fit.
  int fits = 0;
  int tooMany = static_cast<int>(1U << 20U);
  while (tooMany - fits > 1) {
    const int count = std::midpoint(fits, tooMany);
    WebPage page = newPage();
    (page.evaluate(fillThen(count, "0")) ? fits : tooMany) = count;
  }
  for (const std::string_view probe : {"div instanceof HTMLDivElement", "audio instanceof HTMLAudioElement"}) {
    int failures = 0;
    for (int count = fits, successes = 0; successes < 16; --count) {
      ASSERT_GT(count, 0) << probe;
      WebPage page = newPage();
      if (page.evaluate(fillThen(count, probe))) {
        ++successes;
        continue;
      }
      ++failures;
      successes = 0;
      EXPECT_EQ(test::Eval(page, probe), "true") << probe << " after a failure with " << count << " objects";
    }
    EXPECT_GT(failures, 1) << probe;
  }
}

// A rejection without handler is reported, or the call fails: none is lost. Its tracking runs in a QuickJS callback,
// which cannot throw: when it runs out of memory, evaluate() throws std::bad_alloc once the script has returned.
TEST(WebRuntimeTest, RejectionTrackingOutOfMemoryThrows) {
  WebClient client;
  WebPage page = LoadEmpty(client);
  // The tracked rejections (32 bytes each) grow past 4 KiB with a realloc, the only one that large of the script.
  test::FailReallocsOfAtLeast(4096);
  EXPECT_THROW(
      { [[maybe_unused]] const auto result = page.evaluate("for (let i = 0; i < 200; ++i) Promise.reject(i); 0"); },
      std::bad_alloc);
  test::ResetToSysRealloc();
  // The page goes on, and tracks the next rejections.
  const std::size_t errorCount = page.uncaughtErrors().size();
  EXPECT_EQ(test::Eval(page, "Promise.reject('next'); 0"), "0");
  ASSERT_EQ(page.uncaughtErrors().size(), errorCount + 1);
  EXPECT_EQ(page.uncaughtErrors().back(), "Uncaught (in promise) next");
}
#endif

// A replaced value or a removed attribute gives its memory back: changing the same nodes again and again is not a leak.
TEST(WebRuntimeTest, ReplacedDomValuesAreNotALeak) {
  WebClient client(WebClientConfig{.maxScriptMemory = kSmallMemory});
  WebPage page = LoadEmpty(client);
  EXPECT_EQ(
      test::Eval(page,
                 "const text = document.createTextNode(''), p = document.createElement('p'),"
                 "big = 'x'.repeat(1 << 16); for (let i = 0; i < 1000; ++i) { text.data = big; text.nodeValue = '';"
                 "text.textContent = big; text.data = ''; p.setAttribute('a', big); p.setAttribute('a', '');"
                 "p.setAttribute('b', big); p.removeAttribute('b'); p.removeAttribute('missing'); p.textContent = '';"
                 "document.body.insertAdjacentText('beforeend', '') } 'ok'"),
      R"("ok")");
}

// An estimate exceeded by the nodes of very dense HTML: the change is made, then every next one fails.
TEST(WebRuntimeTest, DenseHtmlBeyondTheEstimateFailsTheNextChanges) {
  WebClient client(WebClientConfig{.maxScriptMemory = kSmallMemory});
  WebPage page = LoadEmpty(client);
  // 16 bytes for an element and 7 attributes: about 1.5 KiB of nodes, more than the estimate of 32 bytes per byte.
  EXPECT_TRUE(RunsOutOfMemory(page, "document.body.innerHTML = '<i a b c d e f>'.repeat(4096)"));
  EXPECT_EQ(test::Eval(page, "document.querySelectorAll('i').length"), "4096");
  EXPECT_TRUE(RunsOutOfMemory(page, "document.createElement('p')"));
  EXPECT_TRUE(RunsOutOfMemory(page, "document.createTextNode('t')"));
  EXPECT_TRUE(RunsOutOfMemory(page, "document.createComment('c')"));
  WebPage adjacent = LoadEmpty(client);
  EXPECT_TRUE(
      RunsOutOfMemory(adjacent, "document.body.insertAdjacentHTML('beforeend', '<i a b c d e f>'.repeat(4096))"));
  EXPECT_EQ(test::Eval(adjacent, "document.querySelectorAll('i').length"), "4096");
}

TEST(WebRuntimeTest, PendingTimersAreBounded) {
  WebClient client(WebClientConfig{.maxScriptMemory = kSmallMemory});
  WebPage page = LoadEmpty(client);
  EXPECT_TRUE(
      RunsOutOfMemory(page, "var count = 0; var f = () => {}; for (;;) { setTimeout(f, 1000, 1, 2); ++count }"));
  EXPECT_EQ(test::Eval(page, "count > 10000"), "true");
  // Cleared, they give their memory back.
  EXPECT_EQ(test::Eval(page, "for (let id = 1; id <= count; ++id) clearTimeout(id); setTimeout(f, 1000) > count"),
            "true");
  EXPECT_EQ(test::Eval(page, "for (let i = 0; i < 100000; ++i) clearTimeout(setTimeout(f, 1)); 'ok'"), R"("ok")");
}

TEST(WebRuntimeTest, DocumentWriteIsBounded) {
  WebClient client(WebClientConfig{.maxScriptMemory = kSmallMemory});
  // Each write of 1 KiB is counted as 32 KiB of nodes until it is parsed: 128 writes fit in 4 MiB.
  WebPage page = Load(client,
                      "<p><script>window.count = 0; const s = 'x'.repeat(1024); for (;;) { document.write(s); ++count }"
                      "</script></p>");
  EXPECT_EQ(test::Eval(page, "count"), "128");
  EXPECT_EQ(test::Eval(page, "document.querySelector('p').lastChild.data.length"), "131072");
  ASSERT_EQ(page.uncaughtErrors().size(), 1U);
  EXPECT_TRUE(page.uncaughtErrors()[0].starts_with("InternalError: out of memory")) << page.uncaughtErrors()[0];
  // Also when written in several arguments.
  for (const std::string_view write : {"write", "writeln"}) {
    WebPage arguments = Load(client, "<p><script>const s = 'x'.repeat(1024); for (;;) document." + std::string(write) +
                                         "(s, s)</script></p>");
    ASSERT_EQ(arguments.uncaughtErrors().size(), 1U) << write;
    EXPECT_TRUE(arguments.uncaughtErrors()[0].starts_with("InternalError: out of memory"))
        << write << ": " << arguments.uncaughtErrors()[0];
  }
  // Its nodes, once parsed, beyond the estimate.
  WebPage dense = Load(client, "<p><script>for (;;) document.write('<i a b c d e f>')</script></p>");
  ASSERT_EQ(dense.uncaughtErrors().size(), 2U);
  EXPECT_EQ(dense.uncaughtErrors()[1],
            "aeronet: the output of document.write() exceeds WebClientConfig::maxScriptMemory");
  // The scripts of such an output are inserted, but never run.
  WebPage denseScript = Load(client,
                             "<p><script>document.write('<script>window.ran = 1<\\/script>');"
                             "for (;;) document.write('<i a b c d e f>')</script></p>");
  ASSERT_EQ(denseScript.uncaughtErrors().size(), 2U);
  EXPECT_EQ(denseScript.uncaughtErrors()[1],
            "aeronet: the output of document.write() exceeds WebClientConfig::maxScriptMemory");
  EXPECT_EQ(test::Eval(denseScript, "[document.querySelectorAll('p > script').length, typeof ran]"),
            R"([2,"undefined"])");
}

TEST(WebRuntimeTest, TimeoutsBeyondTheClock) {
  WebClient client;
  WebPage page = Load(client, "<script>window.ran = 1</script>", {.timeout = std::chrono::milliseconds::max()});
  EXPECT_EQ(page.stopReason(), StopReason::StateReached);
  EXPECT_EQ(test::Eval(page, "ran"), "1");
  EXPECT_EQ(page.evaluate("1 + 1", std::chrono::milliseconds::max()).value_or(""), "2");
  // A negative timeout is already over.
  WebPage stopped = Load(client, "<script>for (;;) {}</script>", {.timeout = std::chrono::milliseconds::min()});
  EXPECT_EQ(stopped.stopReason(), StopReason::TimedOut);
}

TEST(WebRuntimeTest, NoLimits) {
  WebClient client(WebClientConfig{
      .maxScriptMemory = std::numeric_limits<std::size_t>::max(),
      .maxScriptStackSize = std::numeric_limits<std::size_t>::max(),
  });
  WebPage page = LoadEmpty(client);
  EXPECT_EQ(test::Eval(page, "new Array(1 << 20).fill(1).length"), "1048576");
}

TEST(WebRuntimeTest, UncaughtErrors) {
  WebClient client;
  WebPage page = Load(client,
                      "<script>throw new Error('first')</script>"
                      "<script>Promise.reject(new RangeError('rejected'))</script>"
                      "<script>const handled = Promise.reject(1); handled.catch(() => {})</script>"
                      "<script>queueMicrotask(() => { throw new Error('in a microtask') })</script>"
                      "<script>window.addEventListener('load', () => { throw new Error('listener') });"
                      "window.addEventListener('load', () => { window.second = true })</script>");
  const auto errors = page.uncaughtErrors();
  ASSERT_EQ(errors.size(), 4U);
  EXPECT_TRUE(errors[0].starts_with("Error: first\n")) << errors[0];
  EXPECT_TRUE(errors[0].contains(page.url())) << errors[0];  // the stack names the document
  EXPECT_TRUE(errors[1].starts_with("Uncaught (in promise) RangeError: rejected")) << errors[1];
  EXPECT_TRUE(errors[2].starts_with("Error: in a microtask")) << errors[2];
  EXPECT_TRUE(errors[3].starts_with("Error: listener")) << errors[3];
  // A throwing listener does not stop the others.
  EXPECT_EQ(test::Eval(page, "second"), "true");
}

TEST(WebRuntimeTest, OnlyTheFirstUncaughtErrorsAreKept) {
  WebClient client;
  WebPage page = Load(client, "<script>for (let i = 0; i < 150; ++i) Promise.reject(i)</script>");
  EXPECT_EQ(page.uncaughtErrors().size(), 100U);
  EXPECT_EQ(page.uncaughtErrors()[0], "Uncaught (in promise) 0");
}

TEST(WebRuntimeTest, LongUncaughtErrorsAreCut) {
  WebClient client;
  // Cut after 8 KiB, at the start of a UTF-8 sequence: 'a' then 4095 two-byte characters.
  WebPage page = Load(client, "<script>throw 'a' + '\xC3\xA9'.repeat(100000)</script><script>throw 'short'</script>");
  ASSERT_EQ(page.uncaughtErrors().size(), 2U);
  const std::string_view cut = page.uncaughtErrors()[0];
  EXPECT_EQ(cut.size(), 8191U + 3U);
  EXPECT_TRUE(cut.ends_with("\xC3\xA9...")) << cut.substr(cut.size() - 8);
  EXPECT_EQ(page.uncaughtErrors()[1], "short");
}

TEST(WebRuntimeTest, Console) {
  WebClient client;
  WebPage page = Load(client,
                      "<script>console.log('text', 1, {a: [1]}, new Error('logged'), undefined, Symbol('s'));"
                      "const cycle = {}; cycle.self = cycle; console.info(cycle); console.debug(); console.warn('w');"
                      "console.error('e'); console.trace(); console.dir({}); console.table([]); console.group('g');"
                      "console.groupEnd(); console.assert(true); console.assert(false, 'assertion'); console.count();"
                      "console.count('x'); console.countReset(); console.time(); console.timeLog(); console.timeEnd();"
                      "</script>");
  EXPECT_TRUE(page.uncaughtErrors().empty());
}

TEST(WebRuntimeTest, ErrorCodes) {
  EXPECT_EQ(ErrcToStr(WebErrc::invalidUrl), "invalid http or https URL");
  EXPECT_EQ(ErrcToStr(WebErrc::requestFailed), "request of the document failed");
  EXPECT_EQ(ErrcToStr(WebErrc::tooManyNavigations), "too many navigations");
  EXPECT_EQ(ErrcToStr(WebErrc::addressBlocked), "server of the document less public than allowed");
  EXPECT_EQ(ErrcToStr(std::bit_cast<WebErrc>(uint8_t{0xFF})), "unknown error");
}

TEST(WebRuntimeTest, InvalidConfigurations) {
  EXPECT_NO_THROW(WebClientConfig{}.validate());
  EXPECT_THROW(WebClient(WebClientConfig{.maxScriptMemory = 0}), std::invalid_argument);
  EXPECT_THROW(WebClient(WebClientConfig{.maxScriptStackSize = 0}), std::invalid_argument);
  EXPECT_THROW(WebClient(WebClientConfig{.maxTimerDelay = std::chrono::milliseconds{-1}}), std::invalid_argument);
  WebClientConfig invalidHttp;
  invalidHttp.http.withProxy("https://proxy.invalid");
  EXPECT_NO_THROW(invalidHttp.validate());
}

// Moves through references: a failed move leaves 'source' usable, which use-after-move checks cannot know.
WebClient MoveConstructed(WebClient& source) { return {std::move(source)}; }

void MoveAssign(WebClient& target, WebClient& source) { target = std::move(source); }

TEST(WebRuntimeTest, ClientMoves) {
  WebClient client;
  {
    WebPage page = LoadEmpty(client);
    // The page points to its client: neither can move while it lives.
    EXPECT_THROW(static_cast<void>(MoveConstructed(client)), std::logic_error);
    WebClient other;
    EXPECT_THROW(MoveAssign(other, client), std::logic_error);
    EXPECT_THROW(MoveAssign(client, other), std::logic_error);
    MoveAssign(client, client);  // a self-assignment moves nothing
    EXPECT_EQ(test::Eval(page, "document.cookie"), R"("")");
    // A failed move leaves both clients as they were.
    WebPage again = LoadEmpty(client);
    page = std::move(again);
  }
  WebClient moved = MoveConstructed(client);
  WebClient assigned;
  MoveAssign(assigned, moved);
  WebPage page = LoadEmpty(assigned);
  EXPECT_EQ(page.status(), http::StatusCodeOK);
  EXPECT_EQ(test::Eval(page, "document.cookie"), R"("")");
  EXPECT_TRUE(assigned.config().http.cookies.enabled());
  EXPECT_TRUE(assigned.httpClient().config().cookies.enabled());
}

#ifdef AERONET_ENABLE_GLAZE
TEST(WebRuntimeTest, EveryScalarFieldOfTheConfigRoundTrips) {
  const test::ScalarsRoundTrip roundTrip = test::MutateScalarsRoundTrip<WebClientConfig>();
  EXPECT_EQ(roundTrip.actual, roundTrip.expected);
}

// The names of the address spaces other than the default one in the configuration files, and an unknown one.
TEST(WebRuntimeTest, AddressSpaceNamesOfTheConfig) {
  struct Named {
    AddressSpace addressSpace;
    std::string_view json;
  };
  for (const Named& named : {
           Named{AddressSpace::Local, R"({"addressSpace":"local"})"},
           Named{AddressSpace::Public, R"({"addressSpace":"public"})"},
       }) {
    WebClientConfig config;
    config.addressSpace = named.addressSpace;
    const std::string json = glz::write_json(config).value_or("");
    EXPECT_TRUE(json.contains(named.json.substr(1, named.json.size() - 2))) << json;
    WebClientConfig read;
    ASSERT_FALSE(glz::read_json(read, named.json)) << named.json;
    EXPECT_EQ(read.addressSpace, named.addressSpace);
  }
  WebClientConfig read;
  EXPECT_TRUE(glz::read_json(read, std::string_view(R"({"addressSpace":"intranet"})")));
  EXPECT_TRUE(glz::read_json(read, std::string_view(R"({"addressSpace":"Public"})")));
}
#endif

}  // namespace

}  // namespace aeronet
