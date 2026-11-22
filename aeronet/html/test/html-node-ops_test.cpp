// The helpers of aeronet_web in aeronet_html: NodesMemory(), UrlResolver, the URL resolution of the pages (WHATWG URL
// parser of lexbor), DecodeScript(), the decoding of their scripts, the selector queries without exception
// (VisitMatches(), MatchSelector()), the insertion points of insertAdjacent*(), and the nodes that HtmlDocument does
// not create (CloneNode(), NewDocumentFragment(), NewElementNs()).
#include "../src/html-node-ops.hpp"

#include <gtest/gtest.h>
#include <lexbor/dom/interfaces/document_fragment.h>
#include <lexbor/dom/interfaces/node.h>

#include <cstddef>
#include <new>
#include <string>
#include <string_view>

#include "../src/html-node-access.hpp"
#include "../src/html-tree-walk.hpp"
#include "aeronet/html-document.hpp"
#include "aeronet/html-element.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/sys-test-support.hpp"
#include "html-test-util.hpp"

namespace aeronet::internal {
namespace {

// Resolved by a new resolver.
std::string ResolveUrl(std::string_view base, std::string_view input) {
  UrlResolver resolver;
  return std::string(resolver.resolve(base, input));
}

TEST(HtmlResolveUrlTest, AgainstABase) {
  constexpr std::string_view kBase = "https://example.com/dir/page?q#f";
  EXPECT_EQ(ResolveUrl(kBase, "../a?b"), "https://example.com/a?b");
  EXPECT_EQ(ResolveUrl(kBase, "x.js"), "https://example.com/dir/x.js");
  EXPECT_EQ(ResolveUrl(kBase, "//other.org/p"), "https://other.org/p");
  EXPECT_EQ(ResolveUrl(kBase, "?other"), "https://example.com/dir/page?other");
  EXPECT_EQ(ResolveUrl(kBase, "#top"), "https://example.com/dir/page?q#top");
  EXPECT_EQ(ResolveUrl(kBase, ""), "https://example.com/dir/page?q");
  EXPECT_EQ(ResolveUrl(kBase, "http://absolute.net"), "http://absolute.net/");
}

TEST(HtmlResolveUrlTest, Serialization) {
  EXPECT_EQ(ResolveUrl({}, "HTTP://EXAMPLE.com:80/a b/../c"), "http://example.com/c");
  EXPECT_EQ(ResolveUrl({}, "https://example.com:8443/é"), "https://example.com:8443/%C3%A9");
  EXPECT_EQ(ResolveUrl({}, "https://bücher.example/"), "https://xn--bcher-kva.example/");
  EXPECT_EQ(ResolveUrl({}, "http://[::1]:8080/"), "http://[::1]:8080/");
}

TEST(HtmlResolveUrlTest, InvalidUrls) {
  EXPECT_EQ(ResolveUrl({}, "relative/path"), "");
  EXPECT_EQ(ResolveUrl({}, ""), "");
  EXPECT_EQ(ResolveUrl({}, "http://[::1/"), "");
  EXPECT_EQ(ResolveUrl("not a url", "x"), "");
  EXPECT_EQ(ResolveUrl("https://example.com/", "http://exa mple.com/"), "");
}

// One resolver for successive bases, as a page resolves its URLs: the last base is kept, and parsed again when it
// changes, also after an invalid base or none.
TEST(HtmlResolveUrlTest, ResolverKeepsTheLastBase) {
  UrlResolver resolver;
  EXPECT_EQ(resolver.resolve("https://a.example/x/", "y"), "https://a.example/x/y");
  const std::string_view again = resolver.resolve("https://a.example/x/", "../z");
  EXPECT_EQ(again, "https://a.example/z");
  EXPECT_EQ(again.data()[again.size()], '\0');
  EXPECT_EQ(resolver.resolve("https://b.example/", "y"), "https://b.example/y");
  EXPECT_EQ(resolver.resolve("not a url", "y"), "");
  EXPECT_EQ(resolver.resolve("not a url", "https://c.example"), "");
  EXPECT_EQ(resolver.resolve({}, "y"), "");
  EXPECT_EQ(resolver.resolve({}, "https://c.example"), "https://c.example/");
  EXPECT_EQ(resolver.resolve("https://b.example/", "?q"), "https://b.example/?q");
  EXPECT_EQ(resolver.resolve({}, "?q"), "");
}

// An input concatenated from parts in the buffer of the resolver, reused: resolved as the whole input.
TEST(HtmlResolveUrlTest, ConcatenatedInput) {
  UrlResolver resolver;
  EXPECT_EQ(resolver.resolve({}, {"https:", "//", "example.com", "/a"}), "https://example.com/a");
  EXPECT_EQ(resolver.resolve("https://example.com/dir/", {"x", ".js", "#", "top"}), "https://example.com/dir/x.js#top");
  EXPECT_EQ(resolver.resolve({}, {}), "");
  EXPECT_EQ(resolver.resolve({}, {"", ""}), "");
  EXPECT_EQ(resolver.resolve({}, {"not", " a url"}), "");
  // A long input grows the buffer, a shorter one reuses it.
  const std::string longPath(4096, 'p');
  EXPECT_EQ(resolver.resolve({}, {"https://example.com/", longPath}), "https://example.com/" + longPath);
  const std::string_view last = resolver.resolve({}, {"https://example.com/", "b"});
  EXPECT_EQ(last, "https://example.com/b");
  // A part can view the last serialization: the parts are copied before it is replaced.
  EXPECT_EQ(resolver.resolve({}, {last, "?q"}), "https://example.com/b?q");
}

std::string Decode(std::string_view bytes, std::string_view contentType, std::string_view charset = {},
                   std::string_view documentEncoding = "UTF-8") {
  RawChars buffer;
  return std::string(DecodeScript(bytes, contentType, charset, documentEncoding, buffer));
}

// The memory of the nodes includes the contents of their templates, nested or not, and the children of a template (only
// added by the DOM).
TEST(HtmlNodesMemoryTest, TemplateContents) {
  HtmlDocument doc("<div id=a><template><template><b>x</b></template><i>yz</i></template>t</div><p>after</p>");
  lxb_dom_node_t* pDiv = HtmlNodeAccess::Node(doc.querySelector("div"));
  lxb_dom_node_t* pTemplate = HtmlNodeAccess::Node(doc.querySelector("template"));
  InsertNode(pTemplate, NewTextNode(pTemplate, "kid"), nullptr);
  // div, its attribute, the templates, b, x, i, yz, kid, t, with the text of the attribute and of the text nodes.
  EXPECT_EQ(NodesMemory(pDiv, pDiv->next), (10 * kNodeMemory) + 1 + 1 + 2 + 3 + 1);
  EXPECT_EQ(NodesMemory(pDiv, nullptr), (12 * kNodeMemory) + 8 + 5);  // with <p>after</p>
  EXPECT_EQ(NodesMemory(pDiv, pDiv), 0U);
}

// The walk is iterative: the templates that scripts nest through their contents have no depth limit.
TEST(HtmlNodesMemoryTest, DeeplyNestedTemplates) {
  HtmlDocument doc("<p>");
  constexpr std::size_t kDepth = 100000;
  lxb_dom_node_t* pTop = HtmlNodeAccess::Node(doc.createElement("template"));
  lxb_dom_node_t* pTemplate = pTop;
  for (std::size_t depth = 1; depth < kDepth; ++depth) {
    lxb_dom_node_t* pNested = HtmlNodeAccess::Node(doc.createElement("template"));
    InsertNode(ChildrenParent(pTemplate), pNested, nullptr);
    pTemplate = pNested;
  }
  EXPECT_EQ(NodesMemory(pTop, nullptr), kDepth * kNodeMemory);
}

TEST(HtmlVisitMatchesTest, StopsWhenTheCallbackReturnsFalse) {
  HtmlDocument doc("<p id=a></p><p id=b></p><p id=c></p>");
  struct Visit {
    std::string ids;
    std::size_t limit;
  } visit{{}, 2};
  const auto collect = [](HtmlElement element, void* pVisit) {
    Visit& state = *static_cast<Visit*>(pVisit);
    state.ids.append(element.attribute("id"));
    return state.ids.size() < state.limit;
  };
  EXPECT_TRUE(VisitMatches(HtmlNodeAccess::Node(doc), "p", collect, &visit));
  EXPECT_EQ(visit.ids, "ab");
  visit = {{}, 10};
  EXPECT_TRUE(VisitMatches(HtmlNodeAccess::Node(doc.body()), "p", collect, &visit));
  EXPECT_EQ(visit.ids, "abc");
}

TEST(HtmlVisitMatchesTest, InvalidSelectorIsAStatus) {
  HtmlDocument doc("<p>x</p>");
  int calls = 0;
  const auto count = [](HtmlElement, void* pCalls) {
    ++*static_cast<int*>(pCalls);
    return true;
  };
  EXPECT_FALSE(VisitMatches(HtmlNodeAccess::Node(doc), "p[", count, &calls));
  EXPECT_FALSE(VisitMatches(HtmlNodeAccess::Node(doc), "", count, &calls));
  EXPECT_EQ(calls, 0);
  // The query engine of the document still works after an invalid selector.
  EXPECT_TRUE(VisitMatches(HtmlNodeAccess::Node(doc), "p", count, &calls));
  EXPECT_EQ(calls, 1);
}

TEST(HtmlAdjacentInsertionPointTest, DomPoints) {
  HtmlDocument doc("<template><i>t</i></template><p><b>x</b>y</p>");
  lxb_dom_node_t* pParagraph = HtmlNodeAccess::Node(doc.querySelector("p"));
  const InsertionPoint inside = AdjacentInsertionPoint(pParagraph, HtmlInsertPosition::AfterBegin);
  EXPECT_EQ(inside.pParent, pParagraph);
  EXPECT_EQ(inside.pReference, pParagraph->first_child);
  EXPECT_TRUE(CanInsertAdjacent(inside));
  EXPECT_EQ(AdjacentInsertionPoint(pParagraph, HtmlInsertPosition::BeforeEnd).pReference, nullptr);
  const InsertionPoint after = AdjacentInsertionPoint(pParagraph, HtmlInsertPosition::AfterEnd);
  EXPECT_EQ(after.pParent, pParagraph->parent);
  EXPECT_EQ(after.pReference, pParagraph->next);
  // Beside the root element: in the document, where HtmlElement does not insert.
  lxb_dom_node_t* pRoot = HtmlNodeAccess::Node(doc.documentElement());
  const InsertionPoint besideRoot = AdjacentInsertionPoint(pRoot, HtmlInsertPosition::BeforeBegin);
  EXPECT_EQ(besideRoot.pParent, HtmlNodeAccess::Node(doc));
  EXPECT_FALSE(CanInsertAdjacent(besideRoot));
  // Beside an element without parent: no parent.
  lxb_dom_node_t* pCreated = HtmlNodeAccess::Node(doc.createElement("div"));
  EXPECT_EQ(AdjacentInsertionPoint(pCreated, HtmlInsertPosition::AfterEnd).pParent, nullptr);
  EXPECT_FALSE(CanInsertAdjacent(AdjacentInsertionPoint(pCreated, HtmlInsertPosition::BeforeBegin)));
  // Beside an element of the contents of a template: in its document fragment.
  lxb_dom_node_t* pInTemplate = ChildrenParent(HtmlNodeAccess::Node(doc.querySelector("template")))->first_child;
  const InsertionPoint inTemplate = AdjacentInsertionPoint(pInTemplate, HtmlInsertPosition::BeforeBegin);
  EXPECT_EQ(inTemplate.pParent->type, LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT);
  EXPECT_TRUE(CanInsertAdjacent(inTemplate));
}

// Beside an element of the contents of a template (reached by the DOM bindings of aeronet_web), the insertions of
// HtmlElement go in its document fragment, HTML parsed as in a <body>.
TEST(HtmlAdjacentInsertionPointTest, BesideAnElementOfTemplateContents) {
  HtmlDocument doc("<template><i>t</i></template>");
  const HtmlElement tmpl = doc.querySelector("template");
  HtmlElement italic = HtmlNodeAccess::Element(ChildrenParent(HtmlNodeAccess::Node(tmpl))->first_child);
  italic.insertAdjacentHtml(HtmlInsertPosition::BeforeBegin, "<b>1</b><td>cell</td>");
  italic.insertAdjacentText(HtmlInsertPosition::AfterEnd, "2");
  italic.insertAdjacentElement(HtmlInsertPosition::AfterEnd, doc.createElement("u"));
  EXPECT_EQ(tmpl.innerHtml(), "<b>1</b>cell<i>t</i><u></u>2");
}

TEST(HtmlDecodeScriptTest, Utf8IsNotCopied) {
  RawChars buffer;
  // Longer than the 8-byte ASCII steps, with a non-ASCII character after them.
  constexpr std::string_view kCode = "const label = 'caf\xC3\xA9';";
  const std::string_view decoded = DecodeScript(kCode, "text/javascript; charset=utf-8", {}, "windows-1252", buffer);
  EXPECT_EQ(decoded.data(), kCode.data());
  EXPECT_EQ(decoded.size(), kCode.size());
  EXPECT_TRUE(buffer.empty());
  EXPECT_EQ(Decode("", "text/javascript"), "");
}

TEST(HtmlDecodeScriptTest, InvalidUtf8IsReplaced) {
  EXPECT_EQ(Decode("var s = 'abcdefgh\xFFz'", "text/javascript"), "var s = 'abcdefgh\xEF\xBF\xBDz'");
  // A sequence truncated by the end of the script.
  EXPECT_EQ(Decode("a\xC3", "text/javascript"), "a\xEF\xBF\xBD");
}

// DecodeUtf8(): UTF-8 only, whatever its byte order mark, which stays.
TEST(HtmlDecodeScriptTest, DecodeUtf8) {
  RawChars buffer;
  constexpr std::string_view kText = "\xEF\xBB\xBF caf\xC3\xA9 and more than 8 bytes";
  EXPECT_EQ(DecodeUtf8(kText, buffer).data(), kText.data());
  EXPECT_TRUE(buffer.empty());
  const std::string_view utf16(
      "\xFF\xFE"
      "a\0",
      4);
  const std::string_view expected(
      "\xEF\xBF\xBD\xEF\xBF\xBD"
      "a\0",
      8);
  // Twice: the second decoding overwrites the first one.
  for (int round = 0; round < 2; ++round) {
    const std::string_view decoded = DecodeUtf8(utf16, buffer);
    EXPECT_EQ(decoded, expected);
    EXPECT_EQ(decoded.data(), buffer.data());
    EXPECT_EQ(buffer.size(), expected.size());
  }
}

TEST(HtmlDecodeScriptTest, EncodingOrder) {
  // 0xA3: '£' in windows-1252, 'Ł' in ISO-8859-2.
  EXPECT_EQ(Decode("\xA3", "text/javascript; charset=windows-1252", "iso-8859-2", "iso-8859-2"), "\xC2\xA3");
  EXPECT_EQ(Decode("\xA3", "text/javascript", " ISO-8859-2 ", "windows-1252"), "\xC5\x81");
  EXPECT_EQ(Decode("\xA3", "text/javascript", {}, "windows-1252"), "\xC2\xA3");
  // Unknown labels are skipped.
  EXPECT_EQ(Decode("\xA3", "text/javascript; charset=bogus", "nope", "windows-1252"), "\xC2\xA3");
  // Without any known encoding: UTF-8.
  EXPECT_EQ(Decode("\xC3\xA9", "text/javascript", "nope", ""), "\xC3\xA9");
}

TEST(HtmlDecodeScriptTest, ByteOrderMarkWins) {
  EXPECT_EQ(Decode("\xEF\xBB\xBFx = '\xC3\xA9'", "text/javascript; charset=windows-1252"), "x = '\xC3\xA9'");
  EXPECT_EQ(Decode(std::string_view("\xFF\xFEx\0=\x00"
                                    "1\0",
                                    8),
                   "text/javascript; charset=windows-1252"),
            "x=1");
  EXPECT_EQ(Decode(std::string_view("\xFE\xFF\0x", 4), "text/javascript", "windows-1252"), "x");
}

// The node of the first element matching 'selector' in 'doc'.
lxb_dom_node_t* NodeOf(const HtmlDocument& doc, std::string_view selector) {
  return HtmlNodeAccess::Node(doc.querySelector(selector));
}

// Whether pRoot or one of its descendants (not those of the template contents) has a user pointer.
bool HasUserPointer(lxb_dom_node_t* pRoot) {
  bool found = pRoot->user != nullptr;
  WalkDescendants(pRoot, [&found](lxb_dom_node_t* pNode) {
    found = found || pNode->user != nullptr;
    return !found;
  });
  return found;
}

constexpr std::string_view kCloneHtml =
    "<div id=src class='a b' data-x=1>text<!--c--><b>bold</b><template id=tpl><i>in</i><template><u>x</u>"
    "</template></template></div>";

TEST(HtmlCloneNodeTest, ShallowAndDeep) {
  HtmlDocument doc(kCloneHtml);
  lxb_dom_node_t* pSource = NodeOf(doc, "#src");
  // lexbor would copy the user pointer of each node: the JavaScript wrapper of a page.
  pSource->user = pSource;
  pSource->first_child->user = pSource;
  const HtmlElement shallow = HtmlNodeAccess::Element(CloneNode(pSource, false));
  EXPECT_EQ(shallow.outerHtml(), R"(<div id="src" class="a b" data-x="1"></div>)");
  EXPECT_EQ(HtmlNodeAccess::Node(shallow)->user, nullptr);
  EXPECT_EQ(HtmlNodeAccess::Node(shallow)->parent, nullptr);
  lxb_dom_node_t* pCopy = CloneNode(pSource, true);
  EXPECT_FALSE(HasUserPointer(pCopy));
  EXPECT_EQ(HtmlNodeAccess::Element(pCopy).outerHtml(), HtmlNodeAccess::Element(pSource).outerHtml());
  EXPECT_EQ(NodesMemory(pCopy, nullptr), NodesMemory(pSource, pSource->next));
  // The contents of the templates are copies too, at each level.
  const HtmlElement copiedTemplate = HtmlNodeAccess::Element(pCopy).querySelector("template");
  EXPECT_EQ(copiedTemplate.innerHtml(), "<i>in</i><template><u>x</u></template>");
  EXPECT_NE(ChildrenParent(HtmlNodeAccess::Node(copiedTemplate)), ChildrenParent(NodeOf(doc, "#tpl")));
  pSource->user = nullptr;
  pSource->first_child->user = nullptr;
}

TEST(HtmlCloneNodeTest, CharacterDataAndFragments) {
  HtmlDocument doc(kCloneHtml);
  lxb_dom_node_t* pSource = NodeOf(doc, "#src");
  lxb_dom_node_t* pText = CloneNode(pSource->first_child, true);
  EXPECT_EQ(pText->type, LXB_DOM_NODE_TYPE_TEXT);
  lxb_dom_node_t* pComment = CloneNode(pSource->first_child->next, false);
  EXPECT_EQ(pComment->type, LXB_DOM_NODE_TYPE_COMMENT);
  // A fragment is copied as a fragment without host, not as the plain node of lexbor.
  lxb_dom_node_t* pContents = ChildrenParent(NodeOf(doc, "#tpl"));
  lxb_dom_node_t* pFragment = CloneNode(pContents, true);
  ASSERT_EQ(pFragment->type, LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT);
  EXPECT_EQ(reinterpret_cast<lxb_dom_document_fragment_t*>(pFragment)->host, nullptr);
  EXPECT_EQ(NodesMemory(pFragment->first_child, nullptr), NodesMemory(pContents->first_child, nullptr));
  EXPECT_EQ(CloneNode(pContents, false)->first_child, nullptr);
  lxb_dom_node_t* pEmpty = NewDocumentFragment(pSource);
  EXPECT_EQ(pEmpty->type, LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT);
  EXPECT_EQ(reinterpret_cast<lxb_dom_document_fragment_t*>(pEmpty)->host, nullptr);
}

// Iterative: a tree deeper than the stack is copied.
TEST(HtmlCloneNodeTest, DeepTrees) {
  HtmlDocument doc("<div id=root></div>");
  HtmlElement parent = doc.querySelector("#root");
  for (int depth = 0; depth < 100000; ++depth) {
    HtmlElement child = doc.createElement(depth % 2 == 0 ? "template" : "p");
    parent.appendChild(child);
    // The children of a template element are its contents: the next level is in them.
    parent = depth % 2 == 0 ? HtmlNodeAccess::Element(ChildrenParent(HtmlNodeAccess::Node(child))) : child;
  }
  lxb_dom_node_t* pRoot = NodeOf(doc, "#root");
  lxb_dom_node_t* pCopy = CloneNode(pRoot, true);
  EXPECT_EQ(NodesMemory(pCopy, nullptr), NodesMemory(pRoot, nullptr));
}

TEST(HtmlNewElementNsTest, Namespaces) {
  HtmlDocument doc("<p></p><svg><circle/></svg>");
  lxb_dom_node_t* pSvg = NewElementNs(NodeOf(doc, "p"), "http://www.w3.org/2000/svg", "svg");
  EXPECT_EQ(NamespaceUri(pSvg), "http://www.w3.org/2000/svg");
  EXPECT_EQ(pSvg->ns, NodeOf(doc, "circle")->ns);
  EXPECT_EQ(HtmlNodeAccess::Element(pSvg).tagName(), "svg");
  lxb_dom_node_t* pPrefixed = NewElementNs(pSvg, "urn:example", "ex:item");
  EXPECT_EQ(NamespaceUri(pPrefixed), "urn:example");
  EXPECT_EQ(HtmlNodeAccess::Element(pPrefixed).tagName(), "ex:item");
  EXPECT_NE(pPrefixed->prefix, 0U);
  lxb_dom_node_t* pPlain = NewElementNs(pSvg, {}, "item");
  EXPECT_EQ(NamespaceUri(pPlain), "");
  EXPECT_EQ(pPlain->prefix, 0U);
  EXPECT_EQ(NamespaceUri(NodeOf(doc, "p")), "http://www.w3.org/1999/xhtml");
}

TEST(HtmlMatchSelectorTest, ElementAndAncestors) {
  HtmlDocument doc("<div class=a><p id=p><b id=b>x</b></p></div>");
  lxb_dom_node_t* pBold = NodeOf(doc, "#b");
  SelectorMatch match = MatchSelector(pBold, "div > p b", false);
  EXPECT_TRUE(match.valid);
  EXPECT_EQ(match.pElement, pBold);
  EXPECT_EQ(MatchSelector(pBold, "p", false).pElement, nullptr);
  EXPECT_EQ(MatchSelector(pBold, "p", true).pElement, NodeOf(doc, "#p"));
  EXPECT_EQ(MatchSelector(pBold, ".a, #p", true).pElement, NodeOf(doc, "#p"));
  EXPECT_EQ(MatchSelector(pBold, "section", true).pElement, nullptr);
  match = MatchSelector(pBold, "p >", true);
  EXPECT_FALSE(match.valid);
  EXPECT_EQ(match.pElement, nullptr);
}

#if AERONET_WANT_MALLOC_OVERRIDES
// Each allocation fails in turn: std::bad_alloc, the source unchanged.
TEST(HtmlCloneNodeTest, MemoryExhaustion) {
  test::ExpectBadAllocOnEachAllocation(
      kCloneHtml,
      [](HtmlDocument& doc) {
        lxb_dom_node_t* pCopy = CloneNode(NodeOf(doc, "#src"), true);
        EXPECT_EQ(HtmlNodeAccess::Element(pCopy).querySelector("template").innerHtml(),
                  "<i>in</i><template><u>x</u></template>");
      },
      [](const HtmlDocument& doc) {
        EXPECT_EQ(doc.querySelector("#src").innerHtml(),
                  "text<!--c--><b>bold</b><template id=\"tpl\"><i>in</i><template><u>x</u></template></template>");
      });
}

TEST(HtmlMatchSelectorTest, MemoryExhaustion) {
  test::ExpectBadAllocOnEachAllocation("<div><p><b>x</b></p></div>", [](HtmlDocument& doc) {
    EXPECT_EQ(MatchSelector(NodeOf(doc, "b"), "div > p", true).pElement, NodeOf(doc, "p"));
  });
}

TEST(HtmlNewElementNsTest, MemoryExhaustion) {
  test::ExpectBadAllocOnEachAllocation("<p></p>", [](HtmlDocument& doc) {
    EXPECT_EQ(NamespaceUri(NewElementNs(NodeOf(doc, "p"), "urn:new-namespace", "x:y")), "urn:new-namespace");
    EXPECT_EQ(NewDocumentFragment(NodeOf(doc, "p"))->type, LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT);
  });
}
#endif

#if AERONET_WANT_MALLOC_OVERRIDES
// Each allocation fails in turn: std::bad_alloc, never a crash nor a wrong URL.
TEST(HtmlResolveUrlTest, MemoryExhaustion) {
  for (int successfulAllocs = 0;; ++successfulAllocs) {
    ASSERT_LT(successfulAllocs, 1000);
    std::string url;
    test::FailNextMalloc(successfulAllocs, 1);
    try {
      UrlResolver resolver;
      // Twice: the second resolution reuses the base, and the memory of the first URL.
      static_cast<void>(resolver.resolve("https://example.com/dir/page", "x"));
      url = resolver.resolve("https://example.com/dir/page", "../a?b");
    } catch (const std::bad_alloc&) {
      test::ResetToSysMalloc();
      continue;
    }
    test::ResetToSysMalloc();
    if (url == "https://example.com/a?b") {
      break;
    }
    EXPECT_EQ(url, "") << successfulAllocs;
  }
}
#endif

}  // namespace
}  // namespace aeronet::internal
