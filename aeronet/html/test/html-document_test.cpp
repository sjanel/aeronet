#include "aeronet/html-document.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "../src/html-parse.hpp"
#include "aeronet/html-element.hpp"
#include "aeronet/sys-test-support.hpp"

namespace aeronet {

namespace {

using internal::kMaxActiveFormattingElements;
using internal::kMaxOpenElements;

// "été" (summer) in UTF-8.
constexpr std::string_view kEteUtf8 = "\xC3\xA9t\xC3\xA9";

std::string BodyText(const HtmlDocument& doc) { return doc.body().textContent(); }

std::string Repeat(std::string_view text, std::size_t count) {
  std::string ret;
  ret.reserve(text.size() * count);
  for (std::size_t idx = 0; idx < count; ++idx) {
    ret.append(text);
  }
  return ret;
}

// The number of elements from the root element to 'element', both included.
std::size_t Depth(HtmlElement element) {
  std::size_t depth = 0;
  for (; element; element = element.parent()) {
    ++depth;
  }
  return depth;
}

std::size_t MaxDepth(const HtmlDocument& doc) {
  std::size_t maxDepth = 0;
  doc.forEach("*", [&maxDepth](HtmlElement element) { maxDepth = std::max(maxDepth, Depth(element)); });
  return maxDepth;
}

std::size_t ChildCount(HtmlElement element) {
  return static_cast<std::size_t>(std::ranges::distance(element.children()));
}

}  // namespace

TEST(HtmlDocument, ParserCreatesHtmlHeadAndBody) {
  const HtmlDocument doc("<p>hello");
  EXPECT_EQ(doc.documentElement().tagName(), "html");
  EXPECT_EQ(doc.head().tagName(), "head");
  EXPECT_EQ(doc.body().tagName(), "body");
  EXPECT_EQ(doc.body().outerHtml(), "<body><p>hello</p></body>");
}

TEST(HtmlDocument, BodyOfAFramesetDocumentIsItsFrameset) {
  const HtmlDocument doc("<frameset><frame src=a></frameset>");
  EXPECT_EQ(doc.head().tagName(), "head");
  ASSERT_TRUE(doc.body());
  EXPECT_EQ(doc.body().tagName(), "frameset");
  EXPECT_EQ(doc.body().querySelector("frame").attribute("src"), "a");
}

TEST(HtmlDocument, BodyReplacedByAFramesetIsNotTheBody) {
  // A <frameset> replaces a <body> without content: the <span> is removed with it.
  const HtmlDocument doc("<span><frameset><frame src=a>");
  EXPECT_EQ(doc.body().tagName(), "frameset");
  EXPECT_EQ(doc.body().parent(), doc.documentElement());
  EXPECT_FALSE(doc.querySelector("span"));
}

TEST(HtmlDocument, BodyAfterCommentAndTextChildrenOfTheRoot) {
  const HtmlDocument doc("<head></head><!--c--> <frameset></frameset>");
  EXPECT_EQ(doc.body().tagName(), "frameset");
  EXPECT_EQ(doc.documentElement().innerHtml(), "<head></head><!--c--> <frameset></frameset>");
}

TEST(HtmlDocument, EmptyInput) {
  const HtmlDocument doc("");
  EXPECT_EQ(doc.encoding(), "UTF-8");
  EXPECT_EQ(doc.documentElement().outerHtml(), "<html><head></head><body></body></html>");
}

TEST(HtmlDocument, MalformedMarkupIsRepairedLikeBrowsers) {
  const HtmlDocument doc("<table><tr><td>a<td>b</table><p>x<div>y</p>");
  const auto cells = doc.querySelectorAll("table > tbody > tr > td");
  ASSERT_EQ(cells.size(), 2U);
  EXPECT_EQ(cells[1].textContent(), "b");
  EXPECT_EQ(doc.body().innerHtml(),
            "<table><tbody><tr><td>a</td><td>b</td></tr></tbody></table><p>x</p><div>y<p></p></div>");
}

TEST(HtmlDocument, InputCanBeReleasedAfterParsing) {
  auto input = std::make_unique<std::string>("<div id='a'>text</div>");
  const HtmlDocument doc(*input);
  input.reset();
  EXPECT_EQ(doc.querySelector("#a").textContent(), "text");
  EXPECT_EQ(doc.querySelector("div").attribute("id"), "a");
}

TEST(HtmlDocument, QueriesIncludeTheRootElement) {
  const HtmlDocument doc("<p>x</p>");
  EXPECT_EQ(doc.querySelector("html"), doc.documentElement());
  EXPECT_EQ(doc.querySelectorAll("*").size(), 4U);  // html, head, body, p
}

TEST(HtmlDocument, MoveConstructionKeepsElementsValid) {
  HtmlDocument first("<span>a</span>");
  const HtmlElement span = first.querySelector("span");
  HtmlDocument second(std::move(first));
  EXPECT_EQ(span.textContent(), "a");
  EXPECT_EQ(second.querySelector("span"), span);
  EXPECT_EQ(second.encoding(), "UTF-8");
}

TEST(HtmlDocument, MoveAssignment) {
  HtmlDocument first("<span>a</span>");
  HtmlDocument second("<b>b</b>");
  // Both documents have a query engine to release.
  EXPECT_TRUE(first.querySelector("span"));
  EXPECT_TRUE(second.querySelector("b"));
  second = std::move(first);
  EXPECT_EQ(second.querySelector("span").textContent(), "a");
  EXPECT_FALSE(second.querySelector("b"));
  // The moved-from document can be assigned to.
  first = HtmlDocument("<i>i</i>");
  EXPECT_EQ(first.querySelector("i").textContent(), "i");
  HtmlDocument& alias = first;
  first = std::move(alias);
  EXPECT_EQ(first.querySelector("i").textContent(), "i");
}

// --- Limits of the tree construction ---

TEST(HtmlDocumentLimits, ElementsBeyondTheDepthLimitBecomeSiblings) {
  static constexpr std::size_t kNested = 2000;
  const HtmlDocument doc(Repeat("<div>", kNested) + "text");
  const auto divs = doc.querySelectorAll("div");
  ASSERT_EQ(divs.size(), kNested);  // no element is lost
  EXPECT_EQ(MaxDepth(doc), kMaxOpenElements);
  EXPECT_EQ(divs.back().textContent(), "text");
  // Below <html> and <body>, the div at the limit and the following ones are children of the one before it.
  const HtmlElement parent = divs[kMaxOpenElements - 4];
  EXPECT_EQ(Depth(parent), kMaxOpenElements - 1);
  EXPECT_EQ(ChildCount(parent), kNested - (kMaxOpenElements - 3));
  EXPECT_EQ(divs.back().parent(), parent);
}

TEST(HtmlDocumentLimits, ContentAfterElementsBeyondTheDepthLimit) {
  // The end tags of the elements closed at the limit find no open element to close: they are ignored.
  const HtmlDocument doc(Repeat("<section>", 600) + "deep" + Repeat("</section>", 600) + "<p id=after>after</p>");
  EXPECT_EQ(doc.querySelectorAll("section").size(), 600U);
  EXPECT_EQ(doc.querySelector("p#after").parent(), doc.body());
  EXPECT_EQ(BodyText(doc), "deepafter");
}

TEST(HtmlDocumentLimits, DepthLimitInTablesAndForeignContent) {
  for (const std::string& html : {Repeat("<table><td>", 1000), "<svg>" + Repeat("<g>", 2000),
                                  "<math>" + Repeat("<mrow>", 2000), Repeat("<template>", 1000),
                                  Repeat("<b><i>", 1000) + "x", Repeat("<ul><li>", 1000), Repeat("<x-custom>", 1000)}) {
    const HtmlDocument doc(html);
    EXPECT_LE(MaxDepth(doc), kMaxOpenElements + 2) << html.substr(0, 20);
  }
}

TEST(HtmlDocumentLimits, ManyNestedElementsParseInLinearTime) {
  // About a minute without the depth limit (quadratic), less than a second with it.
  static constexpr std::size_t kNested = 300000;
  const HtmlDocument doc(Repeat("<div>", kNested) + "x");
  const auto divs = doc.querySelectorAll("div");
  ASSERT_EQ(divs.size(), kNested);
  EXPECT_EQ(Depth(divs.back()), kMaxOpenElements);
}

TEST(HtmlDocumentLimits, EndTagOfAFormattingElementAtTheDepthLimitRemovesALaterOneFirst) {
  // Closed by </p>, b#1, i and b#2 stay in the list of active formatting elements, recreated by the text above the
  // depth limit. </i> closes i and b#2, which stays in the list: at the next start tag, the end tag of the current node
  // b#1 first removes b#2 from the list, then, repeated, closes b#1.
  const HtmlDocument doc("<p><b id=1><i><b id=2></p>" + Repeat("<div>", kMaxOpenElements - 3) + "x</i><span>s</span>");
  const HtmlElement span = doc.querySelector("span");
  EXPECT_EQ(Depth(span), kMaxOpenElements);
  EXPECT_EQ(span.parent().tagName(), "div");
}

TEST(HtmlDocumentLimits, AtMostTheLimitOfMisnestedFormattingElementsAreRecreated) {
  // Closed by </p>, the <b> elements stay in the list of active formatting elements: the text after it is put in new
  // ones, only for the last kMaxActiveFormattingElements.
  static constexpr std::size_t kOpened = kMaxActiveFormattingElements + 4;
  std::string html = "<p>";
  for (std::size_t idx = 0; idx < kOpened; ++idx) {
    html += std::format("<b id={}>", idx);
  }
  const HtmlDocument doc(html + "</p>x");
  EXPECT_EQ(doc.querySelector("p").querySelectorAll("b").size(), kOpened);
  const HtmlElement recreated = doc.body().lastChild();
  EXPECT_EQ(recreated.attribute("id"), "4");
  EXPECT_EQ(recreated.querySelectorAll("b").size(), kMaxActiveFormattingElements - 1);
  EXPECT_EQ(recreated.textContent(), "x");
}

TEST(HtmlDocumentLimits, FormattingElementsLimitIsPerTableCell) {
  // A table cell has its own active formatting elements (after a marker of the list): the 10 <b> outside of the table
  // are recreated after it, and the limit applies to the 20 <i> of the cell only.
  static constexpr std::size_t kOutside = 10;
  static constexpr std::size_t kInCell = kMaxActiveFormattingElements + 4;
  std::string html = "<p>";
  for (std::size_t idx = 0; idx < kOutside; ++idx) {
    html += std::format("<b id=b{}>", idx);
  }
  html += "</p><table><tr><td><p>";
  for (std::size_t idx = 0; idx < kInCell; ++idx) {
    html += std::format("<i id=i{}>", idx);
  }
  const HtmlDocument doc(html + "</p>x</td></tr></table>y");
  const HtmlElement inCell = doc.querySelector("td").lastChild();
  EXPECT_EQ(inCell.attribute("id"), "i4");
  EXPECT_EQ(inCell.querySelectorAll("i").size(), kMaxActiveFormattingElements - 1);
  EXPECT_EQ(inCell.textContent(), "x");
  const HtmlElement afterTable = doc.body().lastChild();
  EXPECT_EQ(afterTable.attribute("id"), "b0");
  EXPECT_EQ(afterTable.querySelectorAll("b").size(), kOutside - 1);
  EXPECT_EQ(afterTable.textContent(), "y");
}

// --- Encoding ---

TEST(HtmlDocumentEncoding, UndeclaredValidUtf8IsUtf8) {
  const HtmlDocument doc(std::string("<p>").append(kEteUtf8));
  EXPECT_EQ(doc.encoding(), "UTF-8");
  EXPECT_EQ(BodyText(doc), kEteUtf8);
}

TEST(HtmlDocumentEncoding, UndeclaredInvalidUtf8IsWindows1252) {
  const HtmlDocument doc("<p>\xE9t\xE9 \x80");
  EXPECT_EQ(doc.encoding(), "windows-1252");
  EXPECT_EQ(BodyText(doc), std::string(kEteUtf8).append(" \xE2\x82\xAC"));  // 0x80 is the euro sign
}

TEST(HtmlDocumentEncoding, UndeclaredUtf8TruncatedAtTheEndIsWindows1252) {
  const HtmlDocument doc("<p>\xC3");
  EXPECT_EQ(doc.encoding(), "windows-1252");
  EXPECT_EQ(BodyText(doc), "\xC3\x83");
}

TEST(HtmlDocumentEncoding, LargeUndeclaredDocuments) {
  std::string utf8 = "<p>";
  std::string latin1 = "<p>";
  for (int idx = 0; idx < 5000; ++idx) {
    utf8.append(kEteUtf8);
    latin1.append("\xE9t\xE9");
  }
  const HtmlDocument utf8Doc(utf8);
  EXPECT_EQ(utf8Doc.encoding(), "UTF-8");
  const HtmlDocument latin1Doc(latin1);
  EXPECT_EQ(latin1Doc.encoding(), "windows-1252");
  EXPECT_EQ(BodyText(latin1Doc), BodyText(utf8Doc));
}

TEST(HtmlDocumentEncoding, ContentTypeCharset) {
  // Labels are matched case-insensitively and mapped to their encoding: latin1 is windows-1252.
  const HtmlDocument doc("<p>\xE9t\xE9", "text/html; charset=\"ISO-8859-1\"");
  EXPECT_EQ(doc.encoding(), "windows-1252");
  EXPECT_EQ(BodyText(doc), kEteUtf8);
}

TEST(HtmlDocumentEncoding, ContentTypeCharsetWinsOverMeta) {
  const HtmlDocument doc(std::string("<meta charset=windows-1252><p>").append(kEteUtf8),
                         "text/html;foo=bar; Charset=utf-8");
  EXPECT_EQ(doc.encoding(), "UTF-8");
  EXPECT_EQ(BodyText(doc), kEteUtf8);
}

TEST(HtmlDocumentEncoding, UnknownContentTypeCharsetFallsBackToMeta) {
  const HtmlDocument doc("<meta charset=\"koi8-r\"><p>\xF0\xD2\xC9", "text/html; charset=nonsense");
  EXPECT_EQ(doc.encoding(), "KOI8-R");
  EXPECT_EQ(BodyText(doc), "\xD0\x9F\xD1\x80\xD0\xB8");  // "При"
}

TEST(HtmlDocumentEncoding, MetaHttpEquiv) {
  const HtmlDocument doc(
      "<head><meta http-equiv=\"Content-Type\" content=\"text/html; charset=Shift_JIS\"></head><p>\x82\xA0",
      "text/html");
  EXPECT_EQ(doc.encoding(), "Shift_JIS");
  EXPECT_EQ(BodyText(doc), "\xE3\x81\x82");  // HIRAGANA LETTER A
}

TEST(HtmlDocumentEncoding, MetaDeclaringUtf16MeansUtf8) {
  const HtmlDocument doc(std::string("<meta charset=utf-16><p>").append(kEteUtf8));
  EXPECT_EQ(doc.encoding(), "UTF-8");
}

TEST(HtmlDocumentEncoding, MetaAfterThePrescannedBytesIsIgnored) {
  std::string html = "<!-- " + std::string(1100, ' ') + " --><meta charset=koi8-r><p>";
  html.append(kEteUtf8);
  const HtmlDocument doc(html);
  EXPECT_EQ(doc.encoding(), "UTF-8");
}

TEST(HtmlDocumentEncoding, Utf8ByteOrderMarkIsRemoved) {
  const HtmlDocument doc(std::string("\xEF\xBB\xBF<p>").append(kEteUtf8), "text/html; charset=windows-1252");
  EXPECT_EQ(doc.encoding(), "UTF-8");
  EXPECT_EQ(BodyText(doc), kEteUtf8);
}

TEST(HtmlDocumentEncoding, Utf16ByteOrderMarks) {
  static constexpr char kLittleEndian[] = "\xFF\xFE<\0p\0>\0\xE9\0t\0\xE9\0";
  const HtmlDocument little(std::string_view(kLittleEndian, sizeof(kLittleEndian) - 1));
  EXPECT_EQ(little.encoding(), "UTF-16LE");
  EXPECT_EQ(BodyText(little), kEteUtf8);

  static constexpr char kBigEndian[] = "\xFE\xFF\0<\0p\0>\0\xE9\0t\0\xE9\xD8\x3D\xDE\x00";
  const HtmlDocument big(std::string_view(kBigEndian, sizeof(kBigEndian) - 1));
  EXPECT_EQ(big.encoding(), "UTF-16BE");
  EXPECT_EQ(BodyText(big), std::string(kEteUtf8).append("\xF0\x9F\x98\x80"));  // U+1F600 from a surrogate pair
}

TEST(HtmlDocumentEncoding, InvalidSequencesOfLegacyEncodingsAreReplaced) {
  // A lead byte of Shift_JIS truncated by the end of the input.
  const HtmlDocument doc("<p>a\x82", "text/html; charset=shift_jis");
  EXPECT_EQ(BodyText(doc), "a\xEF\xBF\xBD");
}

TEST(HtmlDocumentEncoding, Utf8DeclaredInvalidSequencesAreKept) {
  const HtmlDocument doc("<p>a\xFF", "text/html; charset=utf-8");
  EXPECT_EQ(doc.encoding(), "UTF-8");
  EXPECT_EQ(BodyText(doc).size(), 2U);
}

// --- assignedJson ---

TEST(HtmlDocumentAssignedJson, ServerRenderedState) {
  // Shape of a page whose scripts build a table from the embedded state (support.kraken.com).
  const HtmlDocument doc(R"(<html><head>
<script src="/app.js"></script>
<script>window.dataLayer = window.dataLayer || [];</script>
<script>window.__INITIAL_PROPS__={"dehydratedState":{"queries":[{"queryKey":["WITHDRAWAL_METHODS_SUBSCRIBER"],
"state":{"data":{"result":[{"asset":"ZRX","fee":"6.00815","min_amount":"7.20978"}]}}}]}};
window.__INITIAL_PROPS_LOADED__=true;</script>
</head><body><div id="root"></div></body></html>)");
  const std::string_view json = doc.assignedJson("window.__INITIAL_PROPS__");
  EXPECT_TRUE(json.starts_with(R"({"dehydratedState":{"queries":[{"queryKey":["WITHDRAWAL_METHODS_SUBSCRIBER"],)"));
  EXPECT_TRUE(json.ends_with(R"("min_amount":"7.20978"}]}}}]}})"));
  EXPECT_EQ(doc.assignedJson("__INITIAL_PROPS__"), json);
  EXPECT_EQ(doc.assignedJson("window.dataLayer"), "");
}

TEST(HtmlDocumentAssignedJson, OnlyInScripts) {
  const HtmlDocument doc(
      R"(<p>x = {"in": "text"}</p><!-- x = [1] --><script type="text/plain">x = ["script"]</script>)");
  EXPECT_EQ(doc.assignedJson("x"), R"(["script"])");
  EXPECT_EQ(doc.assignedJson("y"), "");
}

TEST(HtmlDocumentAssignedJson, LargeScriptIsOneTextNode) {
  std::string html = "<script>window.__STATE__ = [";
  for (int idx = 0; idx < 200000; ++idx) {
    html += R"({"id":)" + std::to_string(idx) + "},";
  }
  html += "0];</script>";
  const HtmlDocument doc(html);
  const std::string_view json = doc.assignedJson("window.__STATE__");
  EXPECT_TRUE(json.starts_with(R"([{"id":0},)"));
  EXPECT_TRUE(json.ends_with(R"({"id":199999},0])"));
}

#if AERONET_WANT_MALLOC_OVERRIDES
namespace {

// Fails an allocation of the construction of an HtmlDocument, after 'successfulAllocations' ones. Not any allocation:
// lexbor crashes on some of its own allocation failures (lxb_html_document_create cleans up a document it did not
// finish creating).
void ExpectBadAllocOnAllocation(int successfulAllocations, std::string_view html, std::string_view contentType) {
  test::FailNextMalloc(successfulAllocations, 1);
  EXPECT_THROW(HtmlDocument(html, contentType), std::bad_alloc);
  test::ResetToSysMalloc();
  // The same document is then parsed normally.
  EXPECT_TRUE(HtmlDocument(html, contentType).body());
}

}  // namespace

TEST(HtmlDocument, MemoryExhaustionOfTheLexborDocument) {
  // UTF-8 declared by the Content-Type: the first allocation is the one of the lexbor document.
  ExpectBadAllocOnAllocation(0, "<p>x</p>", "text/html; charset=utf-8");
}

TEST(HtmlDocument, MemoryExhaustionOfTheDecodingBuffer) {
  ExpectBadAllocOnAllocation(0, "<p>\xF0\xD2\xC9</p>", "text/html; charset=koi8-r");
}

TEST(HtmlDocument, MemoryExhaustionOfTheMetaPrescan) {
  ExpectBadAllocOnAllocation(0, "<meta charset=koi8-r><p>\xF0\xD2\xC9</p>", "");
  // The prescan state has two arrays: the first one is freed when the second one cannot be allocated.
  ExpectBadAllocOnAllocation(1, "<meta charset=koi8-r><p>\xF0\xD2\xC9</p>", "");
}
#endif

}  // namespace aeronet
