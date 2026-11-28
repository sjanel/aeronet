#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>

#include "../src/html-parse.hpp"
#include "aeronet/format-lite.hpp"
#include "aeronet/html-document.hpp"
#include "aeronet/html-element.hpp"
#include "aeronet/sys-test-support.hpp"
#include "aeronet/vector.hpp"
#include "html-test-util.hpp"

namespace aeronet {

namespace {

using internal::kMaxActiveFormattingElements;
using internal::kMaxAttributes;
using internal::kMaxOpenElements;
using test::AttributeCount;
using test::Attributes;
using test::ChildCount;
using test::Depth;
using test::Repeat;

using enum HtmlInsertPosition;

// The deepest descendant of 'element', counted from 'element' (excluded).
std::size_t MaxDepthBelow(HtmlElement element) {
  std::size_t maxDepth = 0;
  element.forEach("*", [&maxDepth](HtmlElement descendant) { maxDepth = std::max(maxDepth, Depth(descendant)); });
  return maxDepth - Depth(element);
}

}  // namespace

// --- Attributes ---

TEST(HtmlMutationAttributes, SetReplacesTheValueOrAppendsTheAttribute) {
  HtmlDocument doc(R"(<a href="/old" title="t">x</a>)");
  HtmlElement link = doc.querySelector("a");
  link.setAttribute("href", "/new");
  link.setAttribute("DATA-Id", "7");
  EXPECT_EQ(link.attribute("href"), "/new");
  EXPECT_EQ(link.attribute("data-id"), "7");
  EXPECT_EQ(link.outerHtml(), R"(<a href="/new" title="t" data-id="7">x</a>)");
  // Matched in lower case, as by attribute().
  link.setAttribute("TITLE", "");
  EXPECT_TRUE(link.hasAttribute("title"));
  EXPECT_EQ(link.outerHtml(), R"(<a href="/new" title="" data-id="7">x</a>)");
}

TEST(HtmlMutationAttributes, ValueIsEscapedWhenSerialized) {
  HtmlDocument doc("<p>x</p>");
  HtmlElement paragraph = doc.querySelector("p");
  paragraph.setAttribute("title", R"(a "b" & <c>)");
  EXPECT_EQ(paragraph.attribute("title"), R"(a "b" & <c>)");
  // "<" and ">" too, since 2025 (whatwg/html#10665).
  EXPECT_EQ(paragraph.outerHtml(), R"(<p title="a &quot;b&quot; &amp; &lt;c&gt;">x</p>)");
}

TEST(HtmlMutationAttributes, IdAndClassChangesAreSeenBySelectors) {
  HtmlDocument doc(R"(<p id="a" class="x">1</p><p>2</p>)");
  HtmlElement first = doc.querySelector("#a");
  HtmlElement second = first.nextSibling();
  first.setAttribute("id", "b");
  second.setAttribute("id", "a");
  second.setAttribute("class", "y z");
  EXPECT_EQ(doc.querySelector("#a"), second);
  EXPECT_EQ(doc.querySelector("#b"), first);
  EXPECT_EQ(doc.querySelector(".z"), second);
  first.removeAttribute("class");
  EXPECT_FALSE(doc.querySelector(".x"));
  second.removeAttribute("ID");
  EXPECT_FALSE(doc.querySelector("#a"));
  EXPECT_EQ(second.outerHtml(), R"(<p class="y z">2</p>)");
}

// A view without data (default constructed, or the value of an absent attribute) is an empty string for lexbor.
TEST(HtmlMutationAttributes, ViewsWithoutData) {
  HtmlDocument doc("<p id=a>x</p>");
  HtmlElement para = doc.querySelector("p");
  para.setAttribute("copy", para.attribute("absent"));
  para.setAttribute("id", std::string_view{});
  EXPECT_TRUE(para.hasAttribute("copy"));
  EXPECT_FALSE(para.hasAttribute(std::string_view{}));
  EXPECT_TRUE(para.attribute(std::string_view{}).empty());
  para.removeAttribute(std::string_view{});
  EXPECT_EQ(para.outerHtml(), R"(<p id="" copy="">x</p>)");
}

TEST(HtmlMutationAttributes, RemoveAbsentAttributeDoesNothing) {
  HtmlDocument doc(R"(<p title="t">x</p>)");
  HtmlElement paragraph = doc.querySelector("p");
  paragraph.removeAttribute("href");
  paragraph.removeAttribute("");
  EXPECT_EQ(paragraph.outerHtml(), R"(<p title="t">x</p>)");
  paragraph.removeAttribute("title");
  EXPECT_TRUE(paragraph.attributes().empty());
}

TEST(HtmlMutationAttributes, RemovedWhileTheAttributesAreIterated) {
  HtmlDocument doc(R"html(<a onclick="x()" href="/p" onmouseover="y()" title="t" onload="z()">l</a>)html");
  HtmlElement link = doc.querySelector("a");
  vector<HtmlAttribute> removed;
  for (HtmlAttribute attr : link.attributes()) {
    if (attr.name.starts_with("on")) {
      link.removeAttribute(attr.name);
      removed.push_back(attr);
    }
  }
  EXPECT_EQ(link.outerHtml(), R"(<a href="/p" title="t">l</a>)");
  // The views of the removed attributes stay valid.
  ASSERT_EQ(removed.size(), 3U);
  EXPECT_EQ(removed[0].name, "onclick");
  EXPECT_EQ(removed[0].value, "x()");
  EXPECT_EQ(removed[1].name, "onmouseover");
  EXPECT_EQ(removed[2].value, "z()");
  for (HtmlAttribute attr : link.attributes()) {
    link.removeAttribute(attr.name);
  }
  EXPECT_TRUE(link.attributes().empty());
  EXPECT_EQ(link.outerHtml(), "<a>l</a>");
}

TEST(HtmlMutationAttributes, InvalidNamesThrow) {
  HtmlDocument doc("<p>x</p>");
  HtmlElement paragraph = doc.querySelector("p");
  for (std::string_view name : {
           std::string_view{},
           std::string_view{"a b"},
           std::string_view{"a\tb"},
           std::string_view{"a\fb"},
           std::string_view{"a\rb"},
           std::string_view{"a/b"},
           std::string_view{"a=b"},
           std::string_view{"a>b"},
           std::string_view{"a\0b", 3},
       }) {
    EXPECT_FALSE(HtmlElement::IsValidAttributeName(name)) << name;
    EXPECT_THROW(paragraph.setAttribute(name, "v"), std::invalid_argument) << name;
  }
  EXPECT_TRUE(paragraph.attributes().empty());
  // Valid in the DOM, however unusual: the HTML parser reads them back.
  for (std::string_view name : {"@click", "a\"b", "x:y", "\xC3\xA9t\xC3\xA9", "a\vb"}) {
    EXPECT_TRUE(HtmlElement::IsValidAttributeName(name)) << name;
    paragraph.setAttribute(name, "v");
    EXPECT_TRUE(paragraph.hasAttribute(name)) << name;
  }
}

TEST(HtmlMutationAttributes, CaseIsKeptForForeignElements) {
  HtmlDocument doc("<svg></svg>");
  HtmlElement svg = doc.querySelector("svg");
  svg.setAttribute("viewBox", "0 0 1 1");
  EXPECT_EQ(svg.outerHtml(), R"(<svg viewBox="0 0 1 1"></svg>)");
  svg.setAttribute("viewBox", "0 0 2 2");
  EXPECT_EQ(svg.attribute("viewBox"), "0 0 2 2");
}

// --- Text ---

TEST(HtmlMutationText, SetTextContentReplacesTheChildren) {
  HtmlDocument doc("<div>a<b>b</b><!-- c --></div>");
  HtmlElement div = doc.querySelector("div");
  const HtmlElement bold = div.firstChild();
  div.setTextContent("<i>x</i> & y");
  EXPECT_EQ(div.textContent(), "<i>x</i> & y");
  EXPECT_EQ(div.innerHtml(), "&lt;i&gt;x&lt;/i&gt; &amp; y");
  EXPECT_TRUE(div.children().empty());
  // The replaced element is out of the tree, still valid.
  EXPECT_FALSE(bold.parent());
  EXPECT_EQ(bold.outerHtml(), "<b>b</b>");
  EXPECT_FALSE(doc.querySelector("b"));
  div.setTextContent("");
  EXPECT_EQ(div.outerHtml(), "<div></div>");
}

TEST(HtmlMutationText, ViewOfAReplacedTextStaysValid) {
  HtmlDocument doc("<p>before</p>");
  HtmlElement paragraph = doc.querySelector("p");
  std::string buffer;
  const std::string_view before = paragraph.textContent(buffer);
  paragraph.setTextContent("after");
  EXPECT_EQ(before, "before");
  EXPECT_EQ(paragraph.textContent(), "after");
}

TEST(HtmlMutationText, InsertAdjacentText) {
  HtmlDocument doc("<ul><li>b</li></ul>");
  HtmlElement item = doc.querySelector("li");
  item.insertAdjacentText(BeforeBegin, "1<");
  item.insertAdjacentText(AfterBegin, "a");
  item.insertAdjacentText(BeforeEnd, "c");
  item.insertAdjacentText(AfterEnd, "&2");
  item.insertAdjacentText(BeforeEnd, "");
  EXPECT_EQ(doc.querySelector("ul").innerHtml(), "1&lt;<li>abc</li>&amp;2");
  EXPECT_EQ(item.textContent(), "abc");
}

TEST(HtmlMutationText, TextSplitAcrossNodesOfAScriptIsSearchedNodeByNode) {
  HtmlDocument doc("<script>window.state = </script>");
  doc.querySelector("script").insertAdjacentText(BeforeEnd, R"({"a": 1};)");
  EXPECT_EQ(doc.querySelector("script").textContent(), R"(window.state = {"a": 1};)");
  EXPECT_TRUE(doc.assignedJson("window.state").empty());
  doc.querySelector("script").setTextContent(R"(window.state = {"a": 1};)");
  EXPECT_EQ(doc.assignedJson("window.state"), R"({"a": 1})");
}

// --- innerHTML ---

TEST(HtmlMutationInnerHtml, ReplacesTheChildren) {
  HtmlDocument doc("<div><p>old</p></div>");
  HtmlElement div = doc.querySelector("div");
  const HtmlElement old = div.firstChild();
  div.setInnerHtml(R"(<p class="new">a</p>b<br><span>c)");
  EXPECT_EQ(div.innerHtml(), R"(<p class="new">a</p>b<br><span>c</span>)");
  EXPECT_EQ(ChildCount(div), 3U);
  EXPECT_EQ(doc.querySelector(".new").parent(), div);
  EXPECT_FALSE(old.parent());
  EXPECT_EQ(old.textContent(), "old");
  div.setInnerHtml("");
  EXPECT_EQ(div.outerHtml(), "<div></div>");
}

TEST(HtmlMutationInnerHtml, ParsedInTheContextOfTheElement) {
  HtmlDocument doc("<table><tbody></tbody></table><div></div><textarea></textarea><script></script>");
  HtmlElement tbody = doc.querySelector("tbody");
  tbody.setInnerHtml("<tr><td>1</td></tr>");
  EXPECT_EQ(tbody.innerHtml(), "<tr><td>1</td></tr>");
  // Out of a table, the table tags are ignored.
  HtmlElement div = doc.querySelector("div");
  div.setInnerHtml("<tr><td>1</td></tr>");
  EXPECT_EQ(div.innerHtml(), "1");
  // Text in the elements whose contents are text.
  HtmlElement textarea = doc.querySelector("textarea");
  textarea.setInnerHtml("<b>x</b> &amp;");
  EXPECT_TRUE(textarea.children().empty());
  EXPECT_EQ(textarea.textContent(), "<b>x</b> &");
  HtmlElement script = doc.querySelector("script");
  script.setInnerHtml("if (a < b && c) {}");
  EXPECT_EQ(script.innerHtml(), "if (a < b && c) {}");
}

TEST(HtmlMutationInnerHtml, ForeignContext) {
  HtmlDocument doc("<svg></svg>");
  HtmlElement svg = doc.querySelector("svg");
  svg.setInnerHtml(R"(<circle r="1"/><foreignObject><p>x</p></foreignObject>)");
  EXPECT_EQ(svg.innerHtml(), R"(<circle r="1"></circle><foreignObject><p>x</p></foreignObject>)");
  EXPECT_EQ(svg.firstChild().tagName(), "circle");
}

TEST(HtmlMutationInnerHtml, TemplateContents) {
  HtmlDocument doc("<template><b>parsed</b></template>");
  HtmlElement tmpl = doc.querySelector("template");
  EXPECT_EQ(tmpl.innerHtml(), "<b>parsed</b>");
  tmpl.setInnerHtml("<tr><td>x</td></tr>");
  EXPECT_EQ(tmpl.innerHtml(), "<tr><td>x</td></tr>");
  EXPECT_EQ(tmpl.outerHtml(), "<template><tr><td>x</td></tr></template>");
  // The contents are not children, as in the DOM: not navigated, not searched.
  EXPECT_TRUE(tmpl.children().empty());
  EXPECT_FALSE(doc.querySelector("td"));
}

TEST(HtmlMutationInnerHtml, OfTheRootElement) {
  HtmlDocument doc("<title>t</title><p>x</p>");
  HtmlElement root = doc.documentElement();
  root.setInnerHtml("<p>y");
  EXPECT_EQ(root.innerHtml(), "<head></head><body><p>y</p></body>");
  EXPECT_EQ(doc.head(), root.firstChild());
  EXPECT_EQ(doc.body(), root.lastChild());
  EXPECT_EQ(doc.body().textContent(), "y");
}

TEST(HtmlMutationInnerHtml, ReplacedElementsCanBeInsertedAgain) {
  HtmlDocument doc("<ul><li>1</li><li>2</li></ul>");
  HtmlElement list = doc.querySelector("ul");
  const vector<HtmlElement> items = list.querySelectorAll("li");
  list.setInnerHtml("<li>3</li>");
  for (HtmlElement item : items) {
    list.appendChild(item);
  }
  EXPECT_EQ(list.innerHtml(), "<li>3</li><li>1</li><li>2</li>");
}

TEST(HtmlMutationInnerHtml, DepthLimitCountsFromTheFragment) {
  HtmlDocument doc("<div><section></section></div>");
  HtmlElement section = doc.querySelector("section");
  section.setInnerHtml(Repeat("<div>", 2000) + "x");
  // The fragment has an <html> root, as a document: one level less below its context.
  EXPECT_EQ(MaxDepthBelow(section), kMaxOpenElements - 1);
  EXPECT_EQ(section.textContent(), "x");
  EXPECT_EQ(section.querySelectorAll("div").size(), 2000U);
}

TEST(HtmlMutationInnerHtml, MisnestedFormattingElementsLimit) {
  static constexpr std::size_t kOpened = kMaxActiveFormattingElements + 4;
  std::string html = "<p>";
  for (std::size_t idx = 0; idx < kOpened; ++idx) {
    html += Format("<b id={}>", idx);
  }
  HtmlDocument doc("<div></div>");
  HtmlElement div = doc.querySelector("div");
  div.setInnerHtml(html + "</p>x");
  const HtmlElement recreated = div.lastChild();
  EXPECT_EQ(recreated.attribute("id"), "4");
  EXPECT_EQ(recreated.querySelectorAll("b").size(), kMaxActiveFormattingElements - 1);
}

TEST(HtmlMutationInnerHtml, AttributesLimit) {
  HtmlDocument doc("<div></div>");
  HtmlElement div = doc.querySelector("div");
  div.setInnerHtml("<p" + Attributes("a", kMaxAttributes + 1) + ">x</p>");
  HtmlElement para = div.querySelector("p");
  EXPECT_EQ(AttributeCount(para), kMaxAttributes);
  EXPECT_FALSE(para.hasAttribute(Format("a{}", kMaxAttributes)));
  // The limit is the one of the parsing only.
  para.setAttribute("b", "1");
  EXPECT_EQ(AttributeCount(para), kMaxAttributes + 1);
}

TEST(HtmlMutationInnerHtml, LimiterIsRemovedAfterEachParsing) {
  // The parser of the document parses its fragments: each one gets the limits, and only during its parsing.
  HtmlDocument doc("<div id=a></div><div id=b></div>");
  for (int round = 0; round < 3; ++round) {
    for (std::string_view id : {"#a", "#b"}) {
      HtmlElement div = doc.querySelector(id);
      div.setInnerHtml(Repeat("<i>", 1000));
      EXPECT_EQ(MaxDepthBelow(div), kMaxOpenElements - 1);
    }
  }
}

// --- insertAdjacentHTML ---

TEST(HtmlMutationAdjacentHtml, FourPositions) {
  HtmlDocument doc(R"(<ul><li id="a">A</li></ul>)");
  HtmlElement item = doc.querySelector("#a");
  item.insertAdjacentHtml(BeforeBegin, "<li>0</li>");
  item.insertAdjacentHtml(AfterBegin, "<b>1</b><b>2</b>");
  item.insertAdjacentHtml(BeforeEnd, "<i>3</i>");
  item.insertAdjacentHtml(AfterEnd, "<li>4</li>x");
  item.insertAdjacentHtml(BeforeEnd, "");
  EXPECT_EQ(doc.querySelector("ul").innerHtml(), R"(<li>0</li><li id="a"><b>1</b><b>2</b>A<i>3</i></li><li>4</li>x)");
}

TEST(HtmlMutationAdjacentHtml, BesideParsedInTheContextOfTheParent) {
  HtmlDocument doc(R"(<table><tr id="r"><td>1</td></tr></table>)");
  HtmlElement row = doc.querySelector("#r");
  row.insertAdjacentHtml(AfterEnd, "<tr><td>2</td></tr>");
  row.firstChild().insertAdjacentHtml(AfterEnd, "<td>1b</td>");
  EXPECT_EQ(doc.querySelector("tbody").innerHtml(), R"(<tr id="r"><td>1</td><td>1b</td></tr><tr><td>2</td></tr>)");
}

TEST(HtmlMutationAdjacentHtml, InTheRootElementAsInABody) {
  HtmlDocument doc("<p>x</p>");
  HtmlElement root = doc.documentElement();
  root.insertAdjacentHtml(BeforeEnd, "<title>t</title><p>y</p>");
  // Parsed in a <body>: no <head> or <body> of its own, the <title> stays where it is.
  EXPECT_EQ(root.innerHtml(), "<head></head><body><p>x</p></body><title>t</title><p>y</p>");
  doc.head().insertAdjacentHtml(AfterEnd, "<meta charset=utf-8>");
  EXPECT_EQ(doc.head().nextSibling().tagName(), "meta");
}

TEST(HtmlMutationAdjacentHtml, ForeignHtmlElementIsAForeignContext) {
  // Only the <html> element of HTML stands for a <body>: an SVG <html> element is an SVG context.
  HtmlDocument doc("<svg></svg>");
  HtmlElement svg = doc.querySelector("svg");
  svg.setInnerHtml("<html></html>");
  HtmlElement foreignHtml = svg.firstChild();
  ASSERT_EQ(foreignHtml.tagName(), "html");
  // The case of SVG names is adjusted in an SVG context only.
  foreignHtml.insertAdjacentHtml(BeforeEnd, "<foreignobject></foreignobject>");
  EXPECT_EQ(foreignHtml.firstChild().tagName(), "foreignObject");
  doc.body().insertAdjacentHtml(BeforeEnd, "<foreignobject></foreignobject>");
  EXPECT_EQ(doc.body().lastChild().tagName(), "foreignobject");
}

TEST(HtmlMutationAdjacentHtml, BesideTheRootOrAnElementOutOfTheTreeThrows) {
  HtmlDocument doc("<p>x</p>");
  HtmlElement root = doc.documentElement();
  EXPECT_THROW(root.insertAdjacentHtml(BeforeBegin, "<p>y</p>"), std::invalid_argument);
  EXPECT_THROW(root.insertAdjacentHtml(AfterEnd, "<p>y</p>"), std::invalid_argument);
  HtmlElement created = doc.createElement("div");
  EXPECT_THROW(created.insertAdjacentHtml(AfterEnd, "<p>y</p>"), std::invalid_argument);
  EXPECT_THROW(created.insertAdjacentText(BeforeBegin, "y"), std::invalid_argument);
  EXPECT_THROW(created.insertAdjacentElement(BeforeBegin, doc.createElement("p")), std::invalid_argument);
  created.insertAdjacentHtml(BeforeEnd, "<p>y</p>");
  EXPECT_EQ(created.outerHtml(), "<div><p>y</p></div>");
}

// --- Elements ---

TEST(HtmlMutationElements, CreateAndAppend) {
  HtmlDocument doc("<ul><li>1</li></ul>");
  HtmlElement item = doc.createElement("LI");
  EXPECT_EQ(item.tagName(), "li");
  EXPECT_FALSE(item.parent());
  item.setAttribute("class", "new");
  item.setTextContent("2");
  // Queries work out of the tree.
  EXPECT_FALSE(item.querySelector("b"));
  HtmlElement list = doc.querySelector("ul");
  list.appendChild(item);
  EXPECT_EQ(list.innerHtml(), R"(<li>1</li><li class="new">2</li>)");
  EXPECT_EQ(doc.querySelector("li.new"), item);
  EXPECT_EQ(item.parent(), list);
}

TEST(HtmlMutationElements, ElementNames) {
  HtmlDocument doc("");
  for (std::string_view name :
       {"x-widget", "a!", "a\"b", ":x", "_x-1.2", ":a_b", "\xC3\xA9t\xC3\xA9", "\xC3\xA9:a-\xE2\x82\xAC"}) {
    EXPECT_TRUE(HtmlDocument::IsValidElementName(name)) << name;
    EXPECT_EQ(doc.createElement(name).tagName(), name);
  }
  EXPECT_EQ(doc.createElement("My-Widget").tagName(), "my-widget");
  for (std::string_view name : {
           std::string_view{},
           std::string_view{"1a"},
           std::string_view{"-a"},
           std::string_view{"a b"},
           std::string_view{"a/b"},
           std::string_view{"a>b"},
           std::string_view{"a\nb"},
           std::string_view{":a b"},
           std::string_view{"_a!"},
           std::string_view{"a\0b", 3},
       }) {
    EXPECT_FALSE(HtmlDocument::IsValidElementName(name)) << name;
    EXPECT_THROW(static_cast<void>(doc.createElement(name)), std::invalid_argument) << name;
  }
}

TEST(HtmlMutationElements, CreatedTemplateHasContents) {
  HtmlDocument doc("");
  HtmlElement tmpl = doc.createElement("template");
  tmpl.setInnerHtml("<p>x</p>");
  doc.body().appendChild(tmpl);
  EXPECT_EQ(doc.body().innerHtml(), "<template><p>x</p></template>");
}

TEST(HtmlMutationElements, MoveToFourPositions) {
  HtmlDocument doc(R"(<p id="t">text<b>b</b></p><i id="1"></i><i id="2"></i><i id="3"></i><i id="4"></i>)");
  HtmlElement target = doc.querySelector("#t");
  target.insertAdjacentElement(AfterBegin, doc.querySelector("#1"));
  target.insertAdjacentElement(BeforeEnd, doc.querySelector("#2"));
  target.insertAdjacentElement(BeforeBegin, doc.querySelector("#3"));
  target.insertAdjacentElement(AfterEnd, doc.querySelector("#4"));
  // AfterBegin is before the text, not only before the first child element.
  EXPECT_EQ(doc.body().innerHtml(),
            R"(<i id="3"></i><p id="t"><i id="1"></i>text<b>b</b><i id="2"></i></p><i id="4"></i>)");
}

TEST(HtmlMutationElements, MoveNextToItselfKeepsItsPlace) {
  HtmlDocument doc(R"(<i id="1"></i><i id="2"></i><i id="3"></i>)");
  HtmlElement second = doc.querySelector("#2");
  const std::string html = doc.body().innerHtml();
  second.insertAdjacentElement(BeforeBegin, second);
  EXPECT_EQ(doc.body().innerHtml(), html);
  second.insertAdjacentElement(AfterEnd, second);
  EXPECT_EQ(doc.body().innerHtml(), html);
  second.insertAdjacentElement(AfterEnd, second.nextSibling());
  EXPECT_EQ(doc.body().innerHtml(), html);
  second.insertAdjacentElement(BeforeBegin, second.previousSibling());
  EXPECT_EQ(doc.body().innerHtml(), html);
  second.insertAdjacentElement(AfterEnd, second.previousSibling());
  EXPECT_EQ(doc.body().innerHtml(), R"(<i id="2"></i><i id="1"></i><i id="3"></i>)");
}

TEST(HtmlMutationElements, InsertionIntoItselfOrADescendantThrows) {
  HtmlDocument doc("<div><p><b>x</b></p></div>");
  HtmlElement div = doc.querySelector("div");
  HtmlElement bold = doc.querySelector("b");
  EXPECT_THROW(div.appendChild(div), std::invalid_argument);
  EXPECT_THROW(bold.appendChild(div), std::invalid_argument);
  EXPECT_THROW(bold.insertAdjacentElement(AfterEnd, div), std::invalid_argument);
  EXPECT_THROW(div.insertAdjacentElement(AfterBegin, doc.body()), std::invalid_argument);
  EXPECT_EQ(doc.body().innerHtml(), "<div><p><b>x</b></p></div>");
  // Into a subtree out of the tree: the root element can be moved there.
  HtmlElement created = doc.createElement("section");
  created.appendChild(doc.documentElement());
  EXPECT_FALSE(doc.documentElement());
  EXPECT_EQ(created.querySelector("b"), bold);
}

TEST(HtmlMutationElements, InsertionFromAnotherDocumentThrows) {
  HtmlDocument doc("<p>x</p>");
  HtmlDocument other("<b>y</b>");
  EXPECT_THROW(doc.body().appendChild(other.querySelector("b")), std::invalid_argument);
  EXPECT_THROW(doc.body().appendChild(other.createElement("i")), std::invalid_argument);
  EXPECT_EQ(other.body().innerHtml(), "<b>y</b>");
}

TEST(HtmlMutationElements, Remove) {
  HtmlDocument doc("<ul><li>1</li><li>2</li></ul>");
  HtmlElement first = doc.querySelector("li");
  first.remove();
  EXPECT_FALSE(first.parent());
  EXPECT_EQ(doc.querySelectorAll("li").size(), 1U);
  EXPECT_EQ(first.outerHtml(), "<li>1</li>");
  // Out of the tree: nothing to do.
  first.remove();
  doc.querySelector("ul").appendChild(first);
  EXPECT_EQ(doc.querySelector("ul").innerHtml(), "<li>2</li><li>1</li>");
}

TEST(HtmlMutationElements, RemovedHeadBodyAndRootElement) {
  HtmlDocument doc("<title>t</title><p>x</p>");
  HtmlElement root = doc.documentElement();
  doc.body().remove();
  EXPECT_FALSE(doc.body());
  EXPECT_TRUE(doc.head());
  doc.head().remove();
  EXPECT_FALSE(doc.head());
  root.remove();
  EXPECT_FALSE(doc.documentElement());
  EXPECT_FALSE(doc.head());
  EXPECT_FALSE(doc.body());
  EXPECT_FALSE(doc.querySelector("*"));
  EXPECT_EQ(root.outerHtml(), "<html></html>");
}

TEST(HtmlMutationElements, BodyIsAnHtmlElement) {
  // body() is the first <body> or <frameset> child of the root element in the HTML namespace. <head> and <body> start
  // tags leave foreign content, not <frameset>.
  HtmlDocument doc("<svg></svg>");
  HtmlElement svg = doc.querySelector("svg");
  svg.setInnerHtml("<head></head><body></body><frameset></frameset>");
  ASSERT_EQ(ChildCount(svg), 1U);
  const HtmlElement body = doc.body();
  HtmlElement root = doc.documentElement();
  root.insertAdjacentElement(AfterBegin, svg.firstChild());
  EXPECT_EQ(root.firstChild().tagName(), "frameset");
  EXPECT_EQ(doc.body(), body);
}

TEST(HtmlMutationElements, HeadInsertedAgainIsFound) {
  HtmlDocument doc("<p>x</p>");
  HtmlElement root = doc.documentElement();
  doc.head().remove();
  root.insertAdjacentElement(AfterBegin, doc.createElement("div"));
  root.appendChild(doc.createElement("head"));
  EXPECT_EQ(doc.head(), root.lastChild());
}

TEST(HtmlMutationElements, ForeignTemplateHasChildren) {
  // Only an HTML <template> has contents.
  HtmlDocument doc("<svg></svg>");
  HtmlElement svg = doc.querySelector("svg");
  svg.setInnerHtml("<template><circle/></template>");
  HtmlElement tmpl = svg.firstChild();
  EXPECT_EQ(tmpl.tagName(), "template");
  EXPECT_EQ(tmpl.firstChild().tagName(), "circle");
  tmpl.setInnerHtml("<rect/>");
  EXPECT_EQ(tmpl.innerHtml(), "<rect></rect>");
  EXPECT_EQ(tmpl.firstChild().tagName(), "rect");
}

TEST(HtmlMutationElements, QueriedElementsCanBeModifiedAfterTheSearch) {
  HtmlDocument doc("<ul><li>1</li><li class=ad>2</li><li>3</li><li class=ad>4</li></ul>");
  for (HtmlElement item : doc.querySelectorAll("li.ad")) {
    item.remove();
  }
  // Attributes can change during a search.
  doc.forEach("li", [](HtmlElement item) { item.setAttribute("data-text", item.textContent()); });
  EXPECT_EQ(doc.querySelector("ul").innerHtml(), R"(<li data-text="1">1</li><li data-text="3">3</li>)");
}

// The child visited by a children() loop can be removed or moved: the loop goes on with the child that followed it.
TEST(HtmlMutationElements, ChildrenLoopGoesOnAfterTheVisitedChildIsRemovedOrMoved) {
  HtmlDocument doc("<ul><li class=ad>1</li><li class=ad>2</li><li>3</li><li class=ad>4</li></ul><ol><li>z</li></ol>");
  HtmlElement list = doc.querySelector("ul");
  int visited = 0;
  for (HtmlElement item : list.children()) {
    ++visited;
    if (item.hasAttribute("class")) {
      item.remove();
    }
  }
  EXPECT_EQ(visited, 4);
  EXPECT_EQ(list.innerHtml(), "<li>3</li>");

  // Moved before the children of another element: the loop does not go on with them.
  list.setInnerHtml("<li>a</li><li>b</li><li>c</li>");
  HtmlElement ordered = doc.querySelector("ol");
  visited = 0;
  for (HtmlElement item : list.children()) {
    ++visited;
    ordered.insertAdjacentElement(AfterBegin, item);
  }
  EXPECT_EQ(visited, 3);
  EXPECT_TRUE(list.children().empty());
  EXPECT_EQ(ordered.innerHtml(), "<li>c</li><li>b</li><li>a</li><li>z</li>");
}

TEST(HtmlMutationElements, DocumentationExample) {
  // The example of docs/modules/html.md.
  HtmlDocument doc(R"(<ul id="news"><li class="ad">Buy</li><li>Story</li></ul>)");
  for (HtmlElement ad : doc.querySelectorAll("li.ad")) {
    ad.remove();
  }
  HtmlElement list = doc.querySelector("#news");
  list.setAttribute("data-cleaned", "true");
  list.insertAdjacentHtml(HtmlInsertPosition::BeforeEnd, "<li>Added <b>here</b></li>");
  HtmlElement item = doc.createElement("li");
  item.setTextContent("<escaped>");
  list.appendChild(item);
  EXPECT_EQ(
      list.outerHtml(),
      R"(<ul id="news" data-cleaned="true"><li>Story</li><li>Added <b>here</b></li><li>&lt;escaped&gt;</li></ul>)");
}

#if AERONET_WANT_MALLOC_OVERRIDES
using test::ExpectBadAllocOnEachAllocation;

namespace {

// After a failed mutation, the document is still serialized and queried.
void ExpectUsable(const HtmlDocument& doc) {
  EXPECT_FALSE(doc.documentElement().outerHtml().empty());
  EXPECT_TRUE(doc.querySelector("body"));
}

}  // namespace

TEST(HtmlMutationMemory, SetAttributeThrowsBadAlloc) {
  ExpectBadAllocOnEachAllocation(
      R"(<p title="t">x</p>)",
      [](HtmlDocument& doc) {
        // Larger than the text memory of lexbor (48 KiB): allocated by malloc, not taken from its free memory.
        static constexpr std::size_t kLarge = 100000;
        HtmlElement paragraph = doc.querySelector("p");
        paragraph.setAttribute("title", std::string(kLarge, 'v'));
        // A long new name: its allocation also fails.
        const std::string name = "data-" + std::string(kLarge, 'n');
        paragraph.setAttribute(name, std::string(kLarge, 'w'));
        EXPECT_EQ(paragraph.attribute(name).size(), kLarge);
      },
      ExpectUsable);
}

TEST(HtmlMutationMemory, TextThrowsBadAlloc) {
  ExpectBadAllocOnEachAllocation(
      "<ul><li>x</li></ul>",
      [](HtmlDocument& doc) {
        HtmlElement item = doc.querySelector("li");
        item.setTextContent(std::string(1000, 'a'));
        item.insertAdjacentText(AfterEnd, std::string(1000, 'b'));
        EXPECT_EQ(doc.querySelector("ul").textContent().size(), 2000U);
      },
      [](const HtmlDocument& doc) {
        ExpectUsable(doc);
        // The children of the element are unchanged by a failed setTextContent().
        const std::string text = doc.querySelector("li").textContent();
        EXPECT_TRUE(text == "x" || text == std::string(1000, 'a')) << text.size();
      });
}

TEST(HtmlMutationMemory, FragmentParsingThrowsBadAlloc) {
  ExpectBadAllocOnEachAllocation(
      "<div><p>old</p></div><table><tbody></tbody></table>",
      [](HtmlDocument& doc) {
        doc.querySelector("div").setInnerHtml(R"(<p class="a">)" + std::string(1000, 'x') + "</p><b>y</b>");
        doc.querySelector("tbody").insertAdjacentHtml(BeforeEnd, "<tr><td>1</td></tr>");
        EXPECT_EQ(doc.querySelectorAll("p.a, b, td").size(), 3U);
      },
      [](const HtmlDocument& doc) {
        ExpectUsable(doc);
        // Nothing replaced or inserted on failure.
        const std::string div = doc.querySelector("div").innerHtml();
        EXPECT_TRUE(div == "<p>old</p>" || div.starts_with(R"(<p class="a">)")) << div;
      });
}

TEST(HtmlMutationMemory, CreateElementThrowsBadAlloc) {
  ExpectBadAllocOnEachAllocation(
      "<p>x</p>",
      [](HtmlDocument& doc) {
        doc.body().appendChild(doc.createElement("x-new-custom-element"));
        doc.body().appendChild(doc.createElement("template"));
        EXPECT_EQ(doc.body().innerHtml(), "<p>x</p><x-new-custom-element></x-new-custom-element><template></template>");
      },
      ExpectUsable);
}
#endif

}  // namespace aeronet
