# HTML parsing

The HTML module parses HTML documents the way browsers do and queries them with CSS selectors. It is the scraping half of a web client: fetch a page with the [HTTP client](http-client.md), parse it with `HtmlDocument`, read its elements. It runs no JavaScript (see [Pages built by JavaScript](#pages-built-by-javascript)).

It is built on [lexbor](https://github.com/lexbor/lexbor) (Apache-2.0), a C implementation of the WHATWG HTML parser, of the DOM and of CSS selectors, kept out of the public headers. The `AERONET_ENABLE_HTML` CMake option enables the module: `ON` by default when aeronet is the main project, `OFF` when it is a subproject. An installed lexbor 3.x is used when found (Release builds, vcpkg feature `html`, Conan option `with_html`), otherwise lexbor 3.0.1 is downloaded and built as a static library.

| Header | Content |
| --- | --- |
| `<aeronet/html-document.hpp>` | `HtmlDocument` |
| `<aeronet/html-element.hpp>` | `HtmlElement`, `HtmlAttribute`, `HtmlAttributes`, `HtmlChildren`, `HtmlParentNode` (the queries) |
| `<aeronet/css-selector.hpp>` | `CssSelector` |
| `<aeronet/aeronet-html.hpp>` | All of the above (also included by `<aeronet/aeronet.hpp>`) |

## Parse a page

```cpp
#include <aeronet/html-document.hpp>
#include <aeronet/http-client.hpp>

#include <string>
#include <string_view>

HttpClient client;
const auto response = client.get("https://example.com/");
if (response && response->status() == 200) {
  const HtmlDocument doc(response->bodyInMemory(), response->headerValueOrEmpty(http::ContentType));
  for (const HtmlElement link : doc.querySelectorAll("a[href]")) {
    const std::string text = link.textContent();
    const std::string_view href = link.attribute("href");
  }
}
```

The constructor takes the bytes of the document and, optionally, the value of its `Content-Type` header, whose `charset` parameter gives the encoding (see [Encodings](#encodings)). Parsing never fails: malformed markup is repaired as browsers repair it (missing `<html>`, `<head>`, `<body>` and `<tbody>` elements are created, unclosed elements are closed...), so the structure seen by the selectors is the one of the browser's DOM. Only a memory exhaustion throws (`std::bad_alloc`). The parsing time and the number of nodes stay linear in the size of the document, whatever its markup (see [Parsing limits](#parsing-limits)).

The document owns all its nodes: it copies what it keeps from the input, which can be released after the construction. An `HtmlElement` is a handle on an element of the document (one pointer, cheap to copy), valid as long as the document lives, like the `std::string_view` it returns. A document is movable (its elements stay valid) but not copyable.

## Selectors

```cpp
#include <aeronet/css-selector.hpp>
#include <aeronet/html-document.hpp>

#include <string>

const HtmlDocument doc(R"(<table id="fees"><tr><td class="name">0x</td><td>6.00815 ZRX</td></tr>
                                     <tr><td class="name">Aave</td><td>0.00507 AAVE</td></tr></table>)");

// The first match, or an invalid element.
if (const HtmlElement table = doc.querySelector("table#fees")) {
  // All the matches, in document order.
  for (const HtmlElement row : table.querySelectorAll("tr")) {
    const std::string asset = row.querySelector("td.name").textContent();
    const std::string fee = row.querySelector("td:nth-child(2)").textContent();
  }
}

// A selector used many times, compiled once.
const CssSelector names("td.name");
int count = 0;
doc.forEach(names, [&count](HtmlElement) { ++count; });  // no allocation
```

`HtmlDocument` and `HtmlElement` share the queries of `HtmlParentNode`, the ParentNode mixin of the DOM:

| Method | Result |
| --- | --- |
| `querySelector(selector)` | The first descendant matching `selector`, or an invalid `HtmlElement` (`valid()` and `operator bool` are `false`) |
| `querySelectorAll(selector)` | All the matching descendants, in document order, as a `vector<HtmlElement>` |
| `forEach(selector, callback)` | Calls `callback(HtmlElement)` for each matching descendant, without allocating |

Each query takes the selector as text or as a `CssSelector`. A document queries its whole tree (`<html>` included), an element its descendants only, as `Element.querySelectorAll()` of the DOM: the element is not part of its own search, but the selector can match its ancestors (`row.querySelectorAll("table td")` finds the cells of the row). An element matching several selectors of a list (`"td, .name"`) is returned once.

The selectors are those of [Selectors Level 4](https://www.w3.org/TR/selectors-4/) that lexbor implements: type, class, id and attribute selectors (`[a]`, `[a=v]`, `^=`, `$=`, `*=`, `~=`, `|=`, case-insensitive `i`), combinators (descendant, `>`, `+`, `~`), and pseudo-classes such as `:not()`, `:is()`, `:where()`, `:has()`, `:nth-child()`, `:nth-of-type()`, `:first-child`, `:empty`, `:checked`. `:scope` is not supported: `children()` gives the child elements of an element. An invalid selector throws `std::invalid_argument`, at each query when it is given as text, once when a `CssSelector` is compiled.

A selector given as text is parsed at each query, in a memory that the document reuses: after the first queries, a text query only allocates its result (`querySelectorAll()`). A `CssSelector` skips the parsing and works with any document: compile the selectors used in a loop, or on many documents, once.

A `forEach()` callback can query the same document (`doc.forEach("tr", ...)` then `row.querySelectorAll("td")` in the callback), at any depth. An exception thrown by the callback stops the search and is propagated.

## Elements

| Method | Result |
| --- | --- |
| `tagName()` | The local name, in lower case for HTML elements (`"td"`), unlike the upper-case `tagName` of the DOM |
| `attribute(name)` | The value of an attribute (name matched in lower case), empty when absent |
| `hasAttribute(name)` | Whether the attribute is present, to tell an absent attribute from an empty one |
| `attributes()` | A forward range of `HtmlAttribute` (`name`, `value`), in source order |
| `textContent()` | The text of all the descendant text nodes, as the DOM `textContent` (no whitespace collapsing, the contents of `<script>` and `<style>` included), in a single allocation |
| `textContent(buffer)` | The same text as a `std::string_view`, without copy when it is in a single text node, else copied to `buffer` (see [Reusing a buffer](#reusing-a-buffer)) |
| `innerHtml()`, `outerHtml()` | The HTML serialization of the children, and of the element with its children |
| `appendInnerHtml(out)`, `appendOuterHtml(out)` | The same serializations, appended to the `std::string` `out` |
| `parent()`, `firstChild()`, `lastChild()`, `nextSibling()`, `previousSibling()` | The neighbor elements (text and comment nodes are skipped), invalid when there is none |
| `children()` | A forward range of the child elements: `for (HtmlElement cell : row.children())` |

`HtmlDocument` adds `documentElement()`, `head()` and `body()` (the parser always creates them) and `encoding()`. As `document.body` in the DOM, `body()` is the `<frameset>` element of a frameset document.

The text and attribute values are decoded: `&amp;` is `&`, `&eacute;` is `é` (in UTF-8). The `std::string_view` results point into the document.

### Reusing a buffer

`textContent()`, `innerHtml()` and `outerHtml()` return a new `std::string` each time. To read many elements, give them a buffer to reuse instead:

```cpp
#include <aeronet/html-document.hpp>

#include <string>
#include <string_view>

const HtmlDocument doc(R"(<table><tr><td>0x</td><td>6.00815 <b>ZRX</b></td></tr></table>)");

std::string buffer;
for (const HtmlElement cell : doc.querySelectorAll("td")) {
  // "0x" points into the document, "6.00815 ZRX" (two text nodes) into buffer.
  const std::string_view text = cell.textContent(buffer);
}

std::string html = "<!-- row -->";
doc.querySelector("tr").appendOuterHtml(html);  // appended after the comment
```

`textContent(buffer)` does not copy a text that is in a single text node, as in most table cells and links: the view then points into the document, and `buffer` is left untouched. Otherwise the text replaces the contents of `buffer`, which only allocates when it has to grow. Read the returned view, not `buffer`: it stays valid as long as the document lives and `buffer` is not modified, so the next `textContent(buffer)` call can invalidate it. `appendInnerHtml(out)` and `appendOuterHtml(out)` append to `out`, to build a body from several fragments or to reuse one buffer.

## Encodings

The document is decoded to UTF-8 following the encoding sniffing algorithm of the HTML standard:

1. a byte order mark (UTF-8, UTF-16LE, UTF-16BE), removed from the document;
2. else the `charset` parameter of the `Content-Type` given to the constructor;
3. else a `<meta charset>` or `<meta http-equiv="Content-Type">` declaration in the first 1024 bytes (a declared UTF-16 means UTF-8, as for browsers);
4. else UTF-8 when the document is valid UTF-8, windows-1252 otherwise.

The labels are those of the [Encoding standard](https://encoding.spec.whatwg.org/#names-and-labels): `latin1` and `iso-8859-1` are windows-1252, `Shift_JIS`, `EUC-KR`, `GBK`, `gb18030`, `Big5`, `KOI8-R`... are decoded to UTF-8, their invalid sequences being replaced by U+FFFD. `encoding()` returns the canonical name of the encoding used (`"UTF-8"`, `"windows-1252"`, `"Shift_JIS"`...). A document declared as UTF-8 is parsed without copy nor validation: its invalid sequences, if any, are kept as they are.

## Pages built by JavaScript

Many pages are rendered on the server and readable as is. Others build their content in the browser from data that they embed in the page: the scripts of a server-rendered React, Vue or Next.js page often assign their initial state to a global variable. `assignedJson()` returns that JSON text, without running anything:

```cpp
#include <aeronet/html-document.hpp>

#include <string_view>

const HtmlDocument doc(R"(<div id="root"></div>
<script>window.__INITIAL_PROPS__={"fees":[{"asset":"ZRX","fee":"6.00815"}]};</script>)");

const std::string_view json = doc.assignedJson("window.__INITIAL_PROPS__");
// {"fees":[{"asset":"ZRX","fee":"6.00815"}]}: parse it with glaze or any JSON library
```

It searches the inline scripts in document order for an assignment of the given variable to an object or array literal (`variable = {...}` or `[...]`, spaces allowed, comparisons with `==` ignored), and returns the literal from its opening to its matching closing bracket, the brackets in strings being skipped. The variable must not be preceded by an identifier character: `"__INITIAL_PROPS__"` finds `window.__INITIAL_PROPS__ = {...}`, `"data"` does not find `mydata = {...}`. The result is empty when there is no such assignment, or when the first one is not closed. Data in a `<script type="application/json">` element (Next.js `__NEXT_DATA__`) is simply its `textContent()`.

Running the scripts of a page (a JavaScript engine with a DOM and Web APIs) is planned, see the [roadmap](../ROADMAP.md#html-parsing-and-javascript-pages-aeronet_enable_html-aeronet_enable_js).

## Performance

Measured by [html-document_bench.cpp](../../benchmarks/internal/html-document_bench.cpp) on a Release build (x86-64 desktop at 5 GHz), on a table of 1000 to 10000 rows:

| Operation | Cost |
| --- | --- |
| Parsing | 130 to 200 MiB/s (lexbor), about 30 ms for a 4 MB page; decoding windows-1252 or validating an undeclared UTF-8 document adds less than the noise |
| `querySelectorAll()` | About 15 million matched elements per second, with a text or a compiled selector; the text selector costs about 300 ns more per query (its parsing), noticeable on small documents only |
| `querySelector()` | About 400 ns for the first match of a 3-level selector in a table of 1000 rows: the search stops there |
| `textContent()` | About 25 ns for a row of 2 cells, 18 ns with a reused buffer. For a cell whose text is a single node (31 bytes): 17 ns, 5 ns with a buffer (not copied) |
| `innerHtml()` | About 150 ns for a row of 2 cells, 125 ns appended to a reused buffer |
| `assignedJson()` | About 4 GB/s over the scripts: about 1 ms for a 4 MB embedded state |
| Parsing hostile markup | About 7 MiB/s (see [Parsing limits](#parsing-limits)): 70 ms for 100000 nested `<div>` (500 KB), 130 ms for 50000 `<p><b id=N>x</p>` (900 KB, 800000 recreated elements) |

## Parsing limits

The tree construction of HTML is quadratic on some markup, which a page fetched from anywhere can contain on purpose: for most start tags, it searches the open elements for an element "in scope", and before most text and start tags, it recreates the formatting elements (`<b>`, `<font>`...) closed by the end of a block while still open, in each new block. Unbounded, 40000 nested `<div>` (200 KB) took 1.3 s to parse, 300000 more than a minute, and 150 KB of `<p><b id=N>x</p>` (each `<b>` of a different id) created 32 million elements. Two limits keep both linear:

- **Depth**: elements are nested at most 512 levels deep (`<html>` included), the limit of Chromium and WebKit. A start tag at that depth first closes the current element, as if its end tag preceded it: the deeper elements become siblings, and none is lost. The end tags of the closed elements, coming later, close an enclosing element of the same name if there is one, else are ignored: the content that follows such deeply nested elements can end up a few levels higher.
- **Misnested formatting elements**: at most the 16 most recent ones are recreated (per table cell, template...). HTML already forgets the earliest of 4 identical ones (the Noah's Ark clause); beyond 16 of any kind, the earliest are forgotten the same way.

Ordinary pages stay far below both limits. Hostile pages still parse slower than ordinary ones (see [Performance](#performance)), but in proportion to their size.

## Threads

An `HtmlDocument` and its elements must not be used from several threads at the same time, even through `const` methods: the queries share the matching engine of the document. Different documents can be used from different threads, as can a `CssSelector`, which is never modified after its compilation.

## Limitations

- Read-only: there is no DOM mutation (no attribute or node change) for now.
- Elements are nested at most 512 levels deep, and at most 16 misnested formatting elements are recreated: see [Parsing limits](#parsing-limits).
- No JavaScript and no CSS cascade: `<noscript>` contents are parsed as markup, as by a browser with scripting disabled, and the style of an element is not computed.
- `assignedJson()` finds literal assignments only, not data passed to function calls (`self.__next_f.push(...)`).

## Tests

- [html-document_test.cpp](../../aeronet/html/test/html-document_test.cpp): parsing, document accessors (frameset documents), moves, limits of the tree construction, encodings, `assignedJson()`, memory exhaustion.
- [html-element_test.cpp](../../aeronet/html/test/html-element_test.cpp): element accessors, text without copy or in a reused buffer (no allocation), serialization appended to a buffer, navigation, queries, nested queries, exceptions from callbacks.
- [css-selector_test.cpp](../../aeronet/html/test/css-selector_test.cpp): compilation, invalid selectors, reuse across documents.
- [assigned-json_test.cpp](../../aeronet/html/test/assigned-json_test.cpp): the search of an assigned JSON literal.
