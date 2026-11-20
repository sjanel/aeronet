// The JavaScript runtime of a page: built-ins, limits (memory, stack, time), evaluate() results and errors, uncaught
// errors, configuration.
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <expected>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/http-status-code.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/web-client-config.hpp"
#include "aeronet/web-client.hpp"
#include "aeronet/web-page.hpp"
#include "web-test-util.hpp"

#ifdef AERONET_ENABLE_GLAZE
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

TEST(WebRuntimeTest, EndlessMicrotasksAreStoppedByTheTimeout) {
  WebClient client;
  WebPage page = Load(client, "<script>(function loop() { Promise.resolve().then(loop); })()</script>",
                      {.timeout = std::chrono::milliseconds{100}});
  EXPECT_EQ(page.stopReason(), StopReason::TimedOut);
}

TEST(WebRuntimeTest, MemoryLimit) {
  WebClient client(WebClientConfig{.maxScriptMemory = std::size_t{16} << 20});
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
    ASSERT_LT(limit, std::size_t{64} << 20);
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

constexpr std::size_t kSmallMemory = std::size_t{4} << 20;

// Whether 'script' fails with an out of memory error (uncatchable in QuickJS-ng 0.18, as all its internal errors). When
// even the error object does not fit in the memory left, which depends on the exact allocations of the build,
// QuickJS-ng throws null instead (to avoid recursing).
testing::AssertionResult RunsOutOfMemory(WebPage& page, std::string_view script) {
  const auto result = page.evaluate(script);
  if (result) {
    return testing::AssertionFailure() << "the script returned " << *result;
  }
  if (!result.error().starts_with("InternalError: out of memory") && result.error() != "null") {
    return testing::AssertionFailure() << "the script failed with " << result.error();
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
       }) {
    WebPage page = LoadEmpty(client);
    EXPECT_TRUE(RunsOutOfMemory(page, "const big = 'x'.repeat(1 << 16); for (let i = 0;; ++i) " + std::string(grow)))
        << grow;
  }
}

// The arrays returned by the bindings are bounded by the heap: a script keeping them runs out of memory in the middle
// of one, which is freed (QuickJS asserts that no value leaks when the runtime is freed), and the next scripts run.
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
    EXPECT_TRUE(RunsOutOfMemory(page, "var kept = null; for (;;) kept = [kept, " + std::string(array) + "]")) << array;
    EXPECT_EQ(test::Eval(page, "kept = null; document.querySelectorAll('i').length"), "4096") << array;
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

// A rejection without handler is reported, or the call fails: none is lost. Its tracking runs in a QuickJS callback,
// which cannot throw: when it runs out of memory, evaluate() throws std::bad_alloc once the script has returned.
TEST(WebRuntimeTest, RejectionTrackingOutOfMemoryThrows) {
  WebClient client;
  WebPage page = LoadEmpty(client);
  // The tracked rejections (32 bytes each) grow past 4 KiB with a realloc, the only one that large of the script.
  test::FailReallocsOfAtLeast(4096);
  EXPECT_THROW(static_cast<void>(page.evaluate("for (let i = 0; i < 200; ++i) Promise.reject(i); 0")), std::bad_alloc);
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
  WebPage arguments = Load(client, "<p><script>const s = 'x'.repeat(1024); for (;;) document.write(s, s)</script></p>");
  ASSERT_EQ(arguments.uncaughtErrors().size(), 1U);
  EXPECT_TRUE(arguments.uncaughtErrors()[0].starts_with("InternalError: out of memory"))
      << arguments.uncaughtErrors()[0];
  // Its nodes, once parsed, beyond the estimate.
  WebPage dense = Load(client, "<p><script>for (;;) document.write('<i a b c d e f>')</script></p>");
  ASSERT_EQ(dense.uncaughtErrors().size(), 2U);
  EXPECT_EQ(dense.uncaughtErrors()[1],
            "aeronet: the output of document.write() exceeds WebClientConfig::maxScriptMemory");
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
WebClient MoveConstructed(WebClient& source) { return WebClient(std::move(source)); }

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
#endif

}  // namespace
}  // namespace aeronet
