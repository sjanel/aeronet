#include "aeronet/html-element.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <iterator>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/css-selector.hpp"
#include "aeronet/html-document.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

namespace {

constexpr std::string_view kPage = R"(<!DOCTYPE html>
<html><head><title>Fees</title><style>td { color: red }</style></head>
<body>
  <!-- comment -->
  <table id="fees" class="data wide" data-empty="" hidden>
    <tr><th>Asset</th><th>Fee</th></tr>
    <tr class="row"><td class="name">0x</td><td>6.00815 <b>ZRX</b></td></tr>
    <tr class="row"><td class="name">Aave</td><td>0.00507 <b>AAVE</b></td></tr>
  </table>
  text <p id="last">end &amp; <a HREF="https://x.org/?a=1&amp;b=2">link</a></p>
</body></html>)";

class HtmlElementTest : public ::testing::Test {
 protected:
  HtmlDocument doc{kPage};
};

}  // namespace

TEST_F(HtmlElementTest, InvalidElement) {
  const HtmlElement invalid;
  EXPECT_FALSE(invalid.valid());
  EXPECT_FALSE(invalid);
  EXPECT_EQ(invalid, HtmlElement{});
  EXPECT_EQ(doc.querySelector("video"), invalid);
  EXPECT_NE(doc.querySelector("table"), invalid);
}

TEST_F(HtmlElementTest, TagName) {
  EXPECT_EQ(doc.querySelector("#fees").tagName(), "table");
  EXPECT_EQ(doc.querySelector("TD").tagName(), "td");
}

TEST_F(HtmlElementTest, Attributes) {
  const HtmlElement table = doc.querySelector("table");
  EXPECT_EQ(table.attribute("id"), "fees");
  EXPECT_EQ(table.attribute("class"), "data wide");
  EXPECT_EQ(table.attribute("data-empty"), "");
  EXPECT_TRUE(table.hasAttribute("data-empty"));
  EXPECT_TRUE(table.hasAttribute("hidden"));
  EXPECT_EQ(table.attribute("hidden"), "");
  EXPECT_FALSE(table.hasAttribute("style"));
  EXPECT_EQ(table.attribute("style"), "");
  // Names are lower-cased by the parser, and matched in lower case.
  const HtmlElement link = doc.querySelector("a");
  EXPECT_EQ(link.attribute("href"), "https://x.org/?a=1&b=2");
  EXPECT_EQ(link.attribute("HREF"), "https://x.org/?a=1&b=2");
  EXPECT_TRUE(link.hasAttribute("Href"));
}

TEST_F(HtmlElementTest, AttributeRange) {
  const HtmlAttributes attributes = doc.querySelector("table").attributes();
  EXPECT_FALSE(attributes.empty());
  vector<std::pair<std::string_view, std::string_view>> pairs;
  for (const HtmlAttribute attribute : attributes) {
    pairs.emplace_back(attribute.name, attribute.value);
  }
  ASSERT_EQ(pairs.size(), 4U);
  EXPECT_EQ(pairs[0], std::make_pair(std::string_view("id"), std::string_view("fees")));
  EXPECT_EQ(pairs[1], std::make_pair(std::string_view("class"), std::string_view("data wide")));
  EXPECT_EQ(pairs[2], std::make_pair(std::string_view("data-empty"), std::string_view("")));
  EXPECT_EQ(pairs[3], std::make_pair(std::string_view("hidden"), std::string_view("")));

  auto it = attributes.begin();
  EXPECT_EQ((*it++).name, "id");
  EXPECT_EQ((*it).name, "class");
  EXPECT_EQ(std::ranges::distance(attributes), 4);
  static_assert(std::forward_iterator<HtmlAttributes::iterator>);

  EXPECT_TRUE(doc.querySelector("th").attributes().empty());
  EXPECT_EQ(doc.querySelector("th").attributes().begin(), doc.querySelector("th").attributes().end());
}

TEST_F(HtmlElementTest, TextContent) {
  EXPECT_EQ(doc.querySelector("tr.row td:nth-child(2)").textContent(), "6.00815 ZRX");
  EXPECT_EQ(doc.querySelector("#last").textContent(), "end & link");
  EXPECT_EQ(doc.querySelector("th").firstChild(), HtmlElement{});
  // The contents of <style> and <script> are text.
  EXPECT_EQ(doc.head().textContent(), "Feestd { color: red }");
  EXPECT_EQ(HtmlDocument("<p></p>").querySelector("p").textContent(), "");
}

TEST_F(HtmlElementTest, TextContentInSingleNodeIsNotCopied) {
  std::string buffer = "previous";
  const std::string_view name = doc.querySelector("td.name").textContent(buffer);
  EXPECT_EQ(name, "0x");
  // The only text node may be a deeper descendant.
  const std::string_view bold = doc.querySelector("tr.row td:nth-child(2) > b").textContent(buffer);
  EXPECT_EQ(bold, "ZRX");
  const HtmlDocument nested("<div><span><b>deep</b></span><i></i></div>");
  EXPECT_EQ(nested.querySelector("div").textContent(buffer), "deep");
  EXPECT_EQ(nested.querySelector("i").textContent(buffer), "");
  // The views point into the documents: the buffer is untouched.
  EXPECT_EQ(buffer, "previous");
}

TEST_F(HtmlElementTest, TextContentInSeveralNodesIsCopiedToBuffer) {
  std::string buffer = "a previous content, longer than the text";
  const std::string_view fee = doc.querySelector("tr.row td:nth-child(2)").textContent(buffer);
  EXPECT_EQ(fee, "6.00815 ZRX");
  EXPECT_EQ(fee.data(), buffer.data());
  EXPECT_EQ(buffer, "6.00815 ZRX");

  // The buffer is reused without growing, and a view into the document stays valid meanwhile.
  const std::size_t capacity = buffer.capacity();
  const std::string_view name = doc.querySelector("td.name").textContent(buffer);
  EXPECT_EQ(doc.querySelector("#last").textContent(buffer), "end & link");
  EXPECT_EQ(buffer, "end & link");
  EXPECT_EQ(buffer.capacity(), capacity);
  EXPECT_EQ(name, "0x");
  EXPECT_EQ(doc.head().textContent(buffer), "Feestd { color: red }");
}

TEST_F(HtmlElementTest, TextContentWithBufferOfLargeText) {
  const std::string large(100000, 'x');
  const HtmlDocument page("<div><p>" + large + "</p><p>y</p></div>");
  std::string buffer;
  // A single text node, however large, is not copied.
  const std::string_view paragraph = page.querySelector("p").textContent(buffer);
  EXPECT_EQ(paragraph, large);
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(page.querySelector("div").textContent(buffer), large + "y");
  EXPECT_EQ(buffer.size(), large.size() + 1U);
  EXPECT_EQ(page.querySelector("div").textContent(), large + "y");
}

TEST_F(HtmlElementTest, TextContentIncludesCdataSections) {
  const HtmlDocument svg("<svg><text><![CDATA[a<b]]>c</text></svg>");
  EXPECT_EQ(svg.querySelector("text").textContent(), "a<bc");
}

TEST_F(HtmlElementTest, Serialization) {
  const HtmlElement last = doc.querySelector("#last");
  EXPECT_EQ(last.innerHtml(), R"(end &amp; <a href="https://x.org/?a=1&amp;b=2">link</a>)");
  EXPECT_EQ(last.outerHtml(), R"(<p id="last">end &amp; <a href="https://x.org/?a=1&amp;b=2">link</a></p>)");
  EXPECT_EQ(doc.querySelector("th").innerHtml(), "Asset");
}

TEST_F(HtmlElementTest, SerializationAppendsToBuffer) {
  const HtmlElement last = doc.querySelector("#last");
  std::string out = "prefix:";
  last.appendInnerHtml(out);
  out.push_back('|');
  last.appendOuterHtml(out);
  EXPECT_EQ(out, "prefix:" + last.innerHtml() + '|' + last.outerHtml());

  // A large serialization, made of many chunks.
  const HtmlDocument page("<ul>" + std::string(5000, 'z') + "<li>a</li><li>b</li></ul>");
  std::string html;
  page.querySelector("ul").appendOuterHtml(html);
  EXPECT_EQ(html, "<ul>" + std::string(5000, 'z') + "<li>a</li><li>b</li></ul>");
  html.clear();
  page.querySelector("ul").appendInnerHtml(html);
  EXPECT_EQ(html, std::string(5000, 'z') + "<li>a</li><li>b</li>");
}

TEST_F(HtmlElementTest, NavigationSkipsNonElementNodes) {
  const HtmlElement table = doc.querySelector("table");
  const HtmlElement body = doc.body();
  EXPECT_EQ(table.parent(), body);
  EXPECT_EQ(body.parent(), doc.documentElement());
  EXPECT_FALSE(doc.documentElement().parent());
  // Comments and text around the table are skipped.
  EXPECT_EQ(body.firstChild(), table);
  EXPECT_EQ(body.lastChild(), doc.querySelector("#last"));
  EXPECT_EQ(table.nextSibling(), doc.querySelector("#last"));
  EXPECT_FALSE(doc.querySelector("#last").nextSibling());
  EXPECT_EQ(doc.querySelector("#last").previousSibling(), table);
  EXPECT_FALSE(table.previousSibling());
  EXPECT_FALSE(doc.querySelector("b").firstChild());
  EXPECT_FALSE(doc.querySelector("b").lastChild());
  // The parser inserts the <tbody>.
  EXPECT_EQ(table.firstChild().tagName(), "tbody");
}

TEST_F(HtmlElementTest, Children) {
  const HtmlElement row = doc.querySelector("tr.row");
  vector<std::string> cells;
  for (HtmlElement cell : row.children()) {
    cells.push_back(cell.textContent());
  }
  ASSERT_EQ(cells.size(), 2U);
  EXPECT_EQ(cells[0], "0x");
  EXPECT_EQ(cells[1], "6.00815 ZRX");
  EXPECT_FALSE(row.children().empty());
  EXPECT_TRUE(doc.querySelector("th").children().empty());
  auto it = row.children().begin();
  EXPECT_EQ((*it++).textContent(), "0x");
  EXPECT_EQ((*it).textContent(), "6.00815 ZRX");
  EXPECT_EQ(++it, row.children().end());
  static_assert(std::forward_iterator<HtmlChildren::iterator>);
}

TEST_F(HtmlElementTest, QueriesSearchDescendantsOnly) {
  const HtmlElement firstRow = doc.querySelector("tr.row");
  // The element itself is not part of its search.
  EXPECT_FALSE(firstRow.querySelector("tr"));
  EXPECT_EQ(firstRow.querySelectorAll("td").size(), 2U);
  EXPECT_EQ(firstRow.querySelector(CssSelector("b")).textContent(), "ZRX");
  // Descendant combinators may match ancestors outside of the element (as the DOM querySelector).
  EXPECT_EQ(firstRow.querySelectorAll("table td.name").size(), 1U);
}

TEST_F(HtmlElementTest, ScopeIsNotSupported) {
  // lexbor does not parse :scope: children() gives the child elements.
  EXPECT_THROW(static_cast<void>(doc.querySelectorAll(":scope > td")), std::invalid_argument);
}

TEST_F(HtmlElementTest, NoscriptContentIsMarkup) {
  // Parsed as by a browser with scripting disabled.
  const HtmlDocument noscript("<body><noscript><p class=x>no JS</p></noscript></body>");
  EXPECT_EQ(noscript.querySelector("noscript > p.x").textContent(), "no JS");
}

TEST_F(HtmlElementTest, SelectorListReturnsEachElementOnceInDocumentOrder) {
  const auto elements = doc.querySelectorAll("b, td.name, td");
  ASSERT_EQ(elements.size(), 6U);
  EXPECT_EQ(elements[0].textContent(), "0x");
  EXPECT_EQ(elements[1].textContent(), "6.00815 ZRX");
  EXPECT_EQ(elements[2].tagName(), "b");
  EXPECT_EQ(elements[3].textContent(), "Aave");
}

TEST_F(HtmlElementTest, InvalidSelectorTextThrows) {
  EXPECT_THROW(static_cast<void>(doc.querySelector("td[")), std::invalid_argument);
  EXPECT_THROW(static_cast<void>(doc.querySelectorAll("")), std::invalid_argument);
  EXPECT_THROW(doc.forEach("::", [](HtmlElement) {}), std::invalid_argument);
  // The document stays usable.
  EXPECT_EQ(doc.querySelectorAll("td").size(), 4U);
}

TEST_F(HtmlElementTest, ForEach) {
  std::string names;
  doc.forEach("td.name", [&names](HtmlElement cell) { names.append(cell.textContent()).push_back(';'); });
  EXPECT_EQ(names, "0x;Aave;");

  const CssSelector bold("b");
  int count = 0;
  const auto counter = [&count](HtmlElement) { ++count; };
  doc.forEach(bold, counter);  // a const lvalue callback
  EXPECT_EQ(count, 2);

  struct Counter {
    void operator()(HtmlElement /*element*/) { ++calls; }
    int calls = 0;
  } mutableCounter;
  doc.querySelector("table").forEach("tr", mutableCounter);
  EXPECT_EQ(mutableCounter.calls, 3);
}

TEST_F(HtmlElementTest, NestedQueriesInForEach) {
  // Each nesting depth uses its own matching engine and selector memory.
  const CssSelector cellSelector("td");
  vector<std::string> rows;
  doc.forEach("tr.row", [&](HtmlElement row) {
    std::string line;
    row.forEach(cellSelector, [&](HtmlElement cell) {
      line.append(cell.querySelector("b") ? "[" + cell.querySelector("b").textContent() + "]" : cell.textContent());
      // A query with a selector text at depth 3.
      line.append(std::to_string(cell.querySelectorAll("b, i").size()));
    });
    // A query with a selector text at depth 2, after the nested ones.
    line.append(row.querySelector("td.name").textContent());
    rows.push_back(std::move(line));
  });
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0], "0x0[ZRX]10x");
  EXPECT_EQ(rows[1], "Aave0[AAVE]1Aave");
  // The engine of each depth is reused afterwards.
  EXPECT_EQ(doc.querySelectorAll("tr.row td").size(), 4U);
}

TEST_F(HtmlElementTest, ExceptionFromForEachCallbackIsPropagated) {
  int calls = 0;
  EXPECT_THROW(doc.forEach("td",
                           [&calls](HtmlElement) {
                             if (++calls == 2) {
                               throw std::runtime_error("stop");
                             }
                           }),
               std::runtime_error);
  EXPECT_EQ(calls, 2);
  // The search stopped, the document stays usable.
  EXPECT_EQ(doc.querySelectorAll("td").size(), 4U);
  EXPECT_THROW(doc.forEach(CssSelector("td"), [](HtmlElement) { throw std::logic_error("compiled"); }),
               std::logic_error);
}

#if AERONET_WANT_MALLOC_OVERRIDES
namespace {

// Fails each allocation of 'operation' in turn, until it needs no more. The document is parsed before, without
// failure (lexbor crashes on some failed allocations of the parsing, see html-document_test.cpp).
template <class Operation>
void ExpectBadAllocOnEachAllocation(std::string_view html, Operation operation) {
  for (int successfulAllocs = 0;; ++successfulAllocs) {
    const HtmlDocument doc(html);
    test::FailNextMalloc(successfulAllocs, 1);
    try {
      operation(doc);
      test::ResetToSysMalloc();
      return;
    } catch (const std::bad_alloc&) {
      test::ResetToSysMalloc();
    }
    ASSERT_LT(successfulAllocs, 10000);
  }
}

}  // namespace

TEST(HtmlElementMemory, QueriesThrowBadAlloc) {
  std::string html = "<ul>";
  for (int idx = 0; idx < 100; ++idx) {
    html += "<li><b>" + std::to_string(idx) + "</b></li>";
  }
  ExpectBadAllocOnEachAllocation(html, [](const HtmlDocument& doc) {
    EXPECT_EQ(doc.querySelectorAll("li > b").size(), 100U);
    EXPECT_EQ(doc.querySelectorAll(CssSelector("li")).size(), 100U);
    EXPECT_TRUE(doc.querySelector("li:nth-child(50) b"));
    std::size_t nested = 0;
    doc.forEach("li", [&nested](HtmlElement item) { nested += item.querySelectorAll("b").size(); });
    EXPECT_EQ(nested, 100U);
  });
}

TEST(HtmlElementMemory, TextAndSerializationThrowBadAlloc) {
  ExpectBadAllocOnEachAllocation("<div><p>" + std::string(10000, 'x') + "</p><p>y</p></div>",
                                 [](const HtmlDocument& doc) {
                                   const HtmlElement div = doc.querySelector("div");
                                   EXPECT_EQ(div.textContent().size(), 10001U);
                                   EXPECT_EQ(div.innerHtml().size(), 10015U);
                                   EXPECT_EQ(div.outerHtml().size(), 10026U);
                                   std::string buffer;
                                   EXPECT_EQ(div.textContent(buffer).size(), 10001U);
                                   div.appendInnerHtml(buffer);
                                   div.appendOuterHtml(buffer);
                                   EXPECT_EQ(buffer.size(), 10001U + 10015U + 10026U);
                                 });
}

TEST(HtmlElementMemory, TextContentWithReusedBufferDoesNotAllocate) {
  const HtmlDocument doc("<p>single</p><div>several <b>text</b> nodes</div>");
  const HtmlElement paragraph = doc.querySelector("p");
  const HtmlElement div = doc.querySelector("div");
  std::string buffer;
  buffer.reserve(64);
  // Any allocation would throw.
  test::FailNextMalloc(0, 1);
  const std::string_view single = paragraph.textContent(buffer);
  const std::string_view several = div.textContent(buffer);
  test::ResetToSysMalloc();
  EXPECT_EQ(single, "single");
  EXPECT_EQ(several, "several text nodes");
}
#endif

}  // namespace aeronet
