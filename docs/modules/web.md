# JavaScript pages

The web module loads pages the way a browser does without rendering them: `WebClient::navigate()` fetches a document, parses it, runs its scripts over a DOM of the document, follows the navigations they start, and returns the page as its scripts left it. It reads the pages whose content is produced, or only reached, through JavaScript: a waiting room that sends every visitor through a script, a page that builds its table from embedded data. Many pages need none of it: see [Pages built by JavaScript](html.md#pages-built-by-javascript) first, as an `HtmlDocument` costs no script run.

The scripts run on [QuickJS-ng](https://github.com/quickjs-ng/quickjs) (MIT, ES2023 and later), in the process. The `AERONET_ENABLE_JS` CMake option enables the module: `ON` by default when aeronet is the main project, `OFF` when it is a subproject, and always `OFF` without `AERONET_ENABLE_HTML` (the documents are [HtmlDocument](html.md)s) and `AERONET_ENABLE_HTTP_CLIENT`. An installed QuickJS-ng (its `qjs` CMake package) is used when found (Release builds, vcpkg feature `js`), otherwise QuickJS-ng 0.18.0 is downloaded and built as a static library, without its `quickjs-libc` module. The Conan package does not have the module: QuickJS-ng is not in Conan Center.

| Header | Content |
| --- | --- |
| `<aeronet/web-client.hpp>` | `WebClient`, `NavigateOptions`, `LoadState`, `WebErrc` |
| `<aeronet/web-page.hpp>` | `WebPage`, `StopReason` |
| `<aeronet/web-client-config.hpp>` | `WebClientConfig` (and `<aeronet/web-client-config-glaze.hpp>` for its glaze support) |
| `<aeronet/aeronet-web.hpp>` | All of the above (also included by `<aeronet/aeronet.hpp>`) |

## Load a page

```cpp
#include <aeronet/html-element.hpp>
#include <aeronet/web-client.hpp>

#include <chrono>
#include <string>

WebClient web;  // its cookies and storage outlive navigate()

// Until the data is in the document: checked before the scripts run, then after each of them.
auto page = web.navigate("https://shop.example/product/42",
                         {.untilSelector = "script#__NEXT_DATA__", .timeout = std::chrono::seconds{10}});
if (page) {
  if (const HtmlElement data = page->document().querySelector("script#__NEXT_DATA__")) {
    const std::string json = data.textContent();  // parse it with glaze
  }
  // The final URL, after the redirects and the navigations of the scripts.
  const std::string url(page->url());
  // Any expression, its value as JSON text.
  auto title = page->evaluate("document.title");
}
```

`navigate()` returns a `std::expected<WebPage, WebErrc>`. It fails only when the URL, or the URL of a navigation of a page, is not an http or https URL that the HTTP client can request (`invalidUrl`: not http or https, with user info, or port 0), when the request of a document fails (`requestFailed`, logged with the `HttpClientErrc` of the failure), or when the pages start more navigations than `WebClientConfig::maxNavigations` (`tooManyNavigations`). A page is returned whatever its status code (`WebPage::status()`), as browsers show error pages, and whatever its scripts throw: the exceptions that no script caught are in `WebPage::uncaughtErrors()` (the first 100, each cut after 8 KiB, as views valid as long as the page). It throws `std::invalid_argument` for an invalid `untilSelector`, and `std::bad_alloc` (or `std::runtime_error` for a `maxScriptMemory` too small to set up a page) on memory exhaustion.

A `WebPage` holds its document and its JavaScript runtime: `document()` is the [HtmlDocument API](html.md) on the tree that the scripts modified, and the C++ changes made to it are seen by the next scripts. A page refers to its `WebClient` (cookies, storage): it must not outlive it, and the client cannot move while it has pages (a move, or an assignment to it, throws `std::logic_error`). Neither is thread-safe.

## How a page loads

`navigate()` stops at the first of three conditions, given by `WebPage::stopReason()`:

| `StopReason` | Condition |
| --- | --- |
| `SelectorMatched` | An element matches `NavigateOptions::untilSelector`: checked once the document is parsed, before any script runs, then after each task that changed the document |
| `StateReached` | The page reached `NavigateOptions::until`: `LoadState::DomContentLoaded`, `Load` (the default), or `NetworkIdle` |
| `TimedOut` | `NavigateOptions::timeout` (10 s by default) ran out, for the whole navigation: a running script is stopped, the page is returned as it is |

The document is parsed whole, in scripting mode (the contents of `<noscript>` are text, as in browsers that run scripts), then its scripts run in the order of a browser, each one a task:

1. the parser-blocking scripts, inline or external (`src`), in document order, `document.readyState` being `"loading"`;
2. `readyState` becomes `"interactive"` (`readystatechange`), the `defer` scripts run, then `DOMContentLoaded` is dispatched on the document: `LoadState::DomContentLoaded` stops there;
3. the `async` scripts run, `readyState` becomes `"complete"` (`readystatechange`) and `load` is dispatched on the window: `LoadState::Load` stops there;
4. with `LoadState::NetworkIdle`, the timers run until none is left (see [Timers](#timers)).

After each task, the microtasks run (promise reactions, `queueMicrotask()`), the unhandled promise rejections are reported, then the stop conditions and the navigations are checked. Only classic scripts run: a `type` other than a JavaScript MIME type (`application/json`, `module`...) is a data block, and the `nomodule` scripts run, as in a browser without modules. An external script is fetched with the HTTP client of the page, one at a time: a failed request (or a status other than 2xx, or an empty `src`) skips it and dispatches `error` on its element, a loaded one dispatches `load`, which does not reach the window. Its bytes are decoded as browsers do: the encoding of its byte order mark, else of the `charset` of its `Content-Type`, else of its `charset` attribute, else of its document, invalid sequences becoming U+FFFD. The scripts of a document stop at `WebClientConfig::maxScriptBytes`.

A script sees the whole document, already parsed: a browser parsing a slow network stream would show it only the elements before it. `document.write()` during a script inserts its output after the script once it ends, and the scripts it writes run right after it; elsewhere (timers, events, `evaluate()`) it is ignored.

## Timers

`setTimeout()`, `setInterval()`, `requestAnimationFrame()` (16 ms) and `requestIdleCallback()` run on a virtual clock: nothing sleeps. While the document loads, the clock stays at 0, so only the timers due at once run, between the tasks. With `LoadState::NetworkIdle`, after `load`, the clock jumps from timer to timer, in the order of their due times, until none is left. A navigation over a minute of timers takes milliseconds.

Bounds keep endless timers finite: a timer due later than `WebClientConfig::maxTimerDelay` (1 minute) after the start of the document never runs, an interval is cleared after `maxIntervalRuns` (100) runs, and beyond 5 nested levels a timer waits at least 4 ms, as in browsers, so that a `setTimeout(f, 0)` chain moves the clock, also through the microtasks of a timer (`await new Promise(r => setTimeout(r))` in a loop). `Date.now()` and `performance.now()` give the real time.

## Navigations

A script navigates by assigning `location.href`, `window.location`, `document.location`, `location.search` or `location.pathname`, by calling `location.assign()`, `replace()` or `reload()`; a document navigates with `<meta http-equiv="refresh">`. The current task ends first (the last navigation of a task wins), then the page and its runtime are dropped and the new URL, resolved against the base URL of the document (`<base href>`), is loaded with the same options, until the deadline of the first `navigate()` call. A refresh with no delay is followed after `load`; a delayed one only with `NetworkIdle`, after the timers due before it, within `maxTimerDelay`. A refresh of the page itself, and a navigation to a URL other than http and https (`javascript:`, `mailto:`), are not followed. A navigation to a fragment of the document only changes its URL, as does `history.pushState()` (same origin only).

Each navigation counts toward `maxNavigations` (20), beyond which `navigate()` fails: a page that reloads itself forever is an error, not a timeout. The HTTP redirects are followed by the HTTP client and count toward its `maxRedirects` instead. `WebPage::url()` is the final URL of the document, with the fragment of the URL that was asked for.

## Cookies and storage

The `HttpClient` of a `WebClient` (`httpClient()`) always has its cookie jar enabled: the cookies set by the responses and by `document.cookie` live as long as the client, across sites and `navigate()` calls, and go with the next requests, as in the [HTTP client](http-client.md#cookies). `document.cookie` is the non-HTTP API of RFC 6265bis: the `HttpOnly` cookies are hidden from the scripts, which cannot create, replace or delete one.

`localStorage` and `sessionStorage` are kept in memory per origin, as long as the client: a page reads what an earlier page of its origin stored, also from an earlier `navigate()` call. An origin stores at most 5 MiB per storage (`QuotaExceededError` beyond).

## Evaluate a script

`WebPage::evaluate(script, timeout)` runs `script` as a classic script in the page, then its microtasks, and returns its completion value as JSON text (`JSON.stringify`): `"\"text\""`, `"42"`, `"{\"a\":1}"`, or an empty string for a value without JSON representation (`undefined`, a function). It fails with the text of the exception the script throws, also when it runs longer than `timeout` (10 s by default). The timers it sets never run and the navigations it starts are ignored.

## What the scripts can use

| Area | Available |
| --- | --- |
| Language | ES2023 and later, `eval()`, `JSON`, `Proxy`, typed arrays, `Promise`, `WeakRef`, `DOMException`, `atob()`, `btoa()`, `performance.now()`, `queueMicrotask()`. No `SharedArrayBuffer`, `Atomics`, `Intl`, WebAssembly |
| Nodes | `Node`, `Element`, `HTMLElement`, `CharacterData`, `Text`, `Comment`, `Document` with their `instanceof` chains. One wrapper per node: `a === b` holds, and the properties that scripts add to nodes stay |
| Tree | `childNodes`, `children`, `firstChild`, `nextSibling`, `parentNode`, `nextElementSibling`..., `contains()`, `isConnected`, `appendChild()`, `insertBefore()`, `removeChild()`, `replaceChild()`, `remove()`, `append()`, `prepend()`, `before()`, `after()`, `replaceWith()`, `replaceChildren()`, `template.content` |
| Content | `textContent`, `nodeValue`, `data`, `innerHTML`, `outerHTML` (read), `insertAdjacentHTML()`, `insertAdjacentElement()`, `insertAdjacentText()`, `document.write()` |
| Attributes | `getAttribute()`, `setAttribute()`, `removeAttribute()`, `hasAttribute()`, `getAttributeNames()`, `attributes` (a copy), `id`, `className`, `classList`, `dataset`, `style` (values kept, not applied), and `src`, `href`, `name`, `type`, `value`, `hidden`, `disabled`, `async`, `defer`... |
| Queries | `querySelector()`, `querySelectorAll()`, `getElementById()`, `getElementsByTagName()`, `getElementsByClassName()`: arrays, not live collections |
| Document | `createElement()`, `createTextNode()`, `createComment()`, `documentElement`, `head`, `body`, `title`, `readyState`, `currentScript`, `cookie`, `URL`, `characterSet`, `location` |
| Events | `EventTarget`, `Event`, `CustomEvent`, capture and bubble phases, `once`, `preventDefault()`, `stopPropagation()`, the `on*` properties and the inline handler attributes (`<body onload>`) |
| Window | `window`, `self`, `top`, `parent`, `location`, `history` (one entry), `navigator` (`userAgent` from the `User-Agent` header, `language` from `Accept-Language`, `cookieEnabled`), `localStorage`, `sessionStorage`, `console` (to the debug log), the timers |

A DOM operation that the DOM forbids throws its `DOMException` (`HierarchyRequestError`, `NotFoundError`, `InvalidCharacterError`, `SyntaxError` for an invalid selector...).

## Configuration

`WebClient(WebClientConfig)` takes the configuration of its HTTP client and the limits of its pages, listed in the [client configuration reference](../reference/client-configuration.md#javascript-pages): JavaScript heap (256 MiB), native stack (1 MiB), script bytes per document (32 MiB), timer horizon, interval runs, navigations. A memory allocation beyond the heap limit throws an out of memory error in the script, a deeper recursion than the stack limit a `RangeError`. The stack limit must stay below the free stack of the calling thread, or a deep recursion crashes the process: the main thread of a Windows executable has 1 MiB by default, link it with a larger stack (`/STACK:8388608`, as QuickJS-ng does for its own executables) or lower the limit. A JavaScript call takes much more native stack in unoptimized builds (about 9 KiB, 70 KiB with the sanitizers) than in release builds. The native memory that the scripts make a page hold is bounded apart by the same limit: the nodes and attributes they add (about 200 bytes each with their text, an HTML fragment is first checked as 32 bytes per byte), the output of `document.write()` until it is parsed, their pending timers. A replaced value or a removed attribute gives its memory back; a removed node does not, as it stays in the document. With `maxScriptMemory` at 256 MiB, a page cannot add more than about 1.4 million nodes.

## Security

The scripts of the pages run in the process. They have no file, network or process access other than the requests of their page, their memory, stack and time are bounded, and `SharedArrayBuffer` and `Atomics`, the source of several memory-safety bugs of the engine, are removed. A bug of the engine can still be exploited by a hostile page: load untrusted sites from an unprivileged process. The ASAN CI legs build QuickJS-ng with the sanitizers too.

## Performance

Measured by [web-page_bench.cpp](../../benchmarks/internal/web-page_bench.cpp) on a Release build (clang 23, x86-64 desktop, one core):

| Operation | Cost |
| --- | --- |
| Setup of a page (small document) | About 180 µs: QuickJS-ng runtime and context 45 µs, then the parsing, the bindings and the JavaScript part of the Web APIs (16 KB), read back as bytecode |
| Setup of the first page of a `WebClient` | About 830 µs: it compiles the JavaScript part of the Web APIs, for the next pages |
| Native DOM getter or method call | About 20 ns more than a loop iteration without the call |

QuickJS-ng compiles about 20 MB/s of minified code and runs it within about 2 times of V8 on a first run (see the [roadmap](../ROADMAP.md#html-parsing-and-javascript-pages-aeronet_enable_html-aeronet_enable_js)). The external scripts are fetched one at a time.

## Limitations

- No `fetch()` nor `XMLHttpRequest` yet: `LoadState::NetworkIdle` waits for the timers only.
- No ES modules (`type="module"` scripts are skipped), and the scripts inserted by scripts (`createElement('script')`) do not run.
- The subresources of a page are requests of a navigation for the cookie jar: a cross-site script gets the `SameSite=Strict` cookies of its site.
- No layout, rendering, `Intl`, `MutationObserver`, `matchMedia()`, `getComputedStyle()`, `cloneNode()`, `createDocumentFragment()`, `matches()` nor `closest()`.
- A fragment in the `Location` of a redirect is dropped by the HTTP client: the page keeps the fragment of the requested URL.

## Tests

- [web-client_test.cpp](../../aeronet/web/test/web-client_test.cpp): script order and types, events, `document.write()`, external script failures and encodings, script byte limit, timers on the virtual clock (horizon, intervals, nesting, through microtasks, cleared ones), selectors before and after scripts, navigations (location setters, fragments, meta refresh, limits), `HttpOnly` cookies, a waiting room flow across two sites, storage, history, location, navigator, base URL, redirects, forward proxy, failures.
- [web-dom_test.cpp](../../aeronet/web/test/web-dom_test.cpp): wrapper identity and added properties, node types, navigation, text and comment nodes, content, insertions and their errors, queries, the document, `<noscript>` as text, illegal invocations, `classList`, `dataset`, `style`, events.
- [web-runtime_test.cpp](../../aeronet/web/test/web-runtime_test.cpp): built-ins, no shared memory, `evaluate()` results and errors, timeouts (scripts, microtasks, beyond the clock), memory and stack limits, the native memory of the DOM, timers and `document.write()`, setup with too little memory or stack, uncaught errors, console, configuration, client moves.
- [cookie-jar_test.cpp](../../aeronet/client/test/cookie-jar_test.cpp) (`document.cookie`), [http-client-core_test.cpp](../../aeronet/client/test/http-client-core_test.cpp) (`requestUpdating()`), [html-node-ops_test.cpp](../../aeronet/html/test/html-node-ops_test.cpp) (URL resolution, script decoding, the memory of nested templates), [web-url_test.cpp](../../aeronet/web/test/web-url_test.cpp) (document URL and origin).
