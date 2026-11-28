// The helpers of aeronet_web in aeronet_html: NodesMemory(), UrlResolver, the URL resolution of the pages (WHATWG URL
// parser of lexbor), DecodeScript(), the decoding of their scripts, the selector queries without exception
// (VisitMatches(), MatchSelector()), the insertion points of insertAdjacent*(), and the nodes that HtmlDocument does
// not create (CloneNode(), NewDocumentFragment(), NewElementNs()).
#include "../src/html-node-ops.hpp"

#include <gtest/gtest.h>
#include <lexbor/core/dobject.h>
#include <lexbor/core/hash.h>
#include <lexbor/dom/interfaces/document.h>
#include <lexbor/dom/interfaces/document_fragment.h>
#include <lexbor/dom/interfaces/node.h>

#include <array>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "../src/html-encoding.hpp"
#include "../src/html-node-access.hpp"
#include "../src/html-parse.hpp"
#include "../src/html-tree-walk.hpp"
#include "../src/lexbor-utils.hpp"
#include "aeronet/format-lite.hpp"
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
  // The resolved URL is null-terminated: its terminator is the char after the view.
  EXPECT_EQ(std::string_view(again.data(), again.size() + 1U).back(), '\0');
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

// The ancestors of the nodes of template contents go on with their template, at each nesting level: a template
// inserted into its own contents would make a tree that contains itself.
TEST(HtmlHostIncludingAncestorTest, TemplatesOfTheContents) {
  HtmlDocument doc("<div><template><p><template><b>x</b></template></p></template></div>");
  lxb_dom_node_t* pDiv = HtmlNodeAccess::Node(doc.querySelector("div"));
  lxb_dom_node_t* pOuter = pDiv->first_child;
  lxb_dom_node_t* pParagraph = ChildrenParent(pOuter)->first_child;
  lxb_dom_node_t* pInner = pParagraph->first_child;
  lxb_dom_node_t* pBold = ChildrenParent(pInner)->first_child;
  for (const lxb_dom_node_t* pAncestor :
       {pBold, ChildrenParent(pInner), pInner, pParagraph, pOuter, pDiv, HtmlNodeAccess::Node(doc)}) {
    EXPECT_TRUE(IsHostIncludingInclusiveAncestor(pAncestor, pBold));
  }
  EXPECT_FALSE(IsHostIncludingInclusiveAncestor(pBold, pInner));
  EXPECT_FALSE(IsHostIncludingInclusiveAncestor(pInner, pParagraph));
  EXPECT_FALSE(IsHostIncludingInclusiveAncestor(pBold, nullptr));
  // A document fragment without template (DOM createDocumentFragment()) has no ancestor.
  lxb_dom_node_t* pFragment = NewDocumentFragment(pDiv);
  lxb_dom_node_t* pChild = HtmlNodeAccess::Node(doc.createElement("i"));
  InsertNode(pFragment, pChild, nullptr);
  EXPECT_TRUE(IsHostIncludingInclusiveAncestor(pFragment, pChild));
  EXPECT_FALSE(IsHostIncludingInclusiveAncestor(pDiv, pChild));
  // A node without descendants, a template with empty contents too, is only its own ancestor.
  EXPECT_TRUE(IsHostIncludingInclusiveAncestor(pChild, pChild));
  EXPECT_FALSE(IsHostIncludingInclusiveAncestor(pChild, pFragment));
  lxb_dom_node_t* pEmptyTemplate = HtmlNodeAccess::Node(doc.createElement("template"));
  EXPECT_TRUE(IsHostIncludingInclusiveAncestor(pEmptyTemplate, pEmptyTemplate));
  EXPECT_FALSE(IsHostIncludingInclusiveAncestor(pEmptyTemplate, ChildrenParent(pEmptyTemplate)));
}

// insertAdjacentElement() refuses to move a template into its own contents, which the DOM bindings of aeronet_web
// reach.
TEST(HtmlHostIncludingAncestorTest, TemplateInsertedIntoItsOwnContentsThrows) {
  HtmlDocument doc("<div><template><i>t</i></template></div>");
  const HtmlElement tmpl = doc.querySelector("template");
  HtmlElement italic = HtmlNodeAccess::Element(ChildrenParent(HtmlNodeAccess::Node(tmpl))->first_child);
  for (const HtmlInsertPosition position : {
           HtmlInsertPosition::BeforeBegin,
           HtmlInsertPosition::AfterBegin,
           HtmlInsertPosition::BeforeEnd,
           HtmlInsertPosition::AfterEnd,
       }) {
    EXPECT_THROW(italic.insertAdjacentElement(position, tmpl), std::invalid_argument);
  }
  // Nothing moved.
  EXPECT_EQ(doc.querySelector("div").innerHtml(), "<template><i>t</i></template>");
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

// --- Name tables of the documents (GrowNameTable() of html-parse.cpp) ---

// The lexbor document of 'doc'.
lxb_dom_document_t& LexborDocument(const HtmlDocument& doc) { return *HtmlNodeAccess::Node(doc)->owner_document; }

// The number of names interned in the tables of tags and attributes of 'doc'.
struct InternedNames {
  explicit InternedNames(const HtmlDocument& doc)
      : tags(lexbor_dobject_allocated(LexborDocument(doc).tags->entries)),
        attributes(lexbor_dobject_allocated(LexborDocument(doc).attrs->entries)) {}

  bool operator==(const InternedNames&) const = default;

  std::size_t tags;
  std::size_t attributes;
};

// Interns names of each variant of lexbor in 'parent', short and long ones (stored apart from their entry): in lower
// case by the parsing, in upper case by the tag names of HTML elements, as given by the attributes of an SVG element.
void InternNamesOfEachVariant(HtmlElement parent) {
  parent.insertAdjacentHtml(HtmlInsertPosition::BeforeEnd,
                            "<x-a an-attribute-with-a-long-name=1></x-a><x-a-custom-element-with-a-long-name>"
                            "</x-a-custom-element-with-a-long-name><svg><g></g></svg>");
  EXPECT_EQ(DomTagName(HtmlNodeAccess::Node(parent.querySelectorAll("x-a").back())), "X-A");
  EXPECT_EQ(DomTagName(HtmlNodeAccess::Node(parent.querySelectorAll("x-a-custom-element-with-a-long-name").back())),
            "X-A-CUSTOM-ELEMENT-WITH-A-LONG-NAME");
  HtmlElement svgGroup = parent.querySelectorAll("g").back();
  svgGroup.setAttribute("mixedCase", "1");
  svgGroup.setAttribute("An-Attribute-With-A-Mixed-Case-Long-Name", "1");
}

// The number of names interned in each table of 'doc' is at most kMaxNamesPerBucket per bucket: the tables grew.
void ExpectGrownTables(const HtmlDocument& doc) {
  for (const lexbor_hash_t* pTable : {LexborDocument(doc).tags, LexborDocument(doc).attrs}) {
    EXPECT_LE(lexbor_dobject_allocated(pTable->entries), kMaxNamesPerBucket * pTable->table_size);
  }
}

TEST(HtmlNameTableTest, NamesOfEachVariantAreFoundAfterAGrowth) {
  HtmlDocument doc("<p></p>");
  const lxb_dom_document_t& document = LexborDocument(doc);
  InternNamesOfEachVariant(doc.body());
  const std::size_t tagBuckets = document.tags->table_size;
  const std::size_t attributeBuckets = document.attrs->table_size;
  // Distinct names, in a fragment too small to reserve buckets first: the tables grow after its chunk.
  std::string names;
  for (std::size_t idx = 0; idx < 2 * kMaxNamesPerBucket * tagBuckets; ++idx) {
    FormatTo(names, "<y-{} b{}>", idx, idx);
  }
  ASSERT_LT(names.size(), tagBuckets * kHtmlBytesPerNameBucket);
  doc.body().insertAdjacentHtml(HtmlInsertPosition::BeforeEnd, names);
  ASSERT_GT(document.tags->table_size, tagBuckets);
  ASSERT_GT(document.attrs->table_size, attributeBuckets);
  // The same names again are found: none is interned twice.
  const InternedNames before(doc);
  InternNamesOfEachVariant(doc.body());
  doc.body().insertAdjacentHtml(HtmlInsertPosition::BeforeEnd, names);
  EXPECT_EQ(InternedNames(doc), before);
}

TEST(HtmlNameTableTest, ParsingReservesBucketsForItsInput) {
  const std::string html = "<p>" + std::string(1024 * kHtmlBytesPerNameBucket, 'x') + "</p>";
  const HtmlDocument doc(html);
  EXPECT_EQ(LexborDocument(doc).tags->table_size, html.size() / kHtmlBytesPerNameBucket);
  EXPECT_EQ(LexborDocument(doc).attrs->table_size, html.size() / kHtmlBytesPerNameBucket);
}

TEST(HtmlNameTableTest, ManyDistinctNamesParseInLinearTime) {
  // Each distinct name of a tag or an attribute is interned in a table of the document, which would keep 128 buckets
  // without its growth (2.7 MB of these attributes took 7 s, quadratic). The tables grow between the chunks of the
  // input, inside a tag too: the tokenizer interns its attribute names as it reads them (more than a name per 64 bytes
  // here, beyond the buckets reserved first).
  static constexpr std::size_t kCount = 1UL << 16U;
  std::string attributes;
  std::string tags;
  std::string oneTag = "<i";
  for (std::size_t idx = 0; idx < kCount; ++idx) {
    FormatTo(attributes, "<i a{}>", idx);
    FormatTo(tags, "<x-{}></x-{}>", idx, idx);
    FormatTo(oneTag, " a{}", idx);
  }
  oneTag += "><p id=after>";
  ASSERT_GT(oneTag.size(), 16 * kParseChunkSize);
  const HtmlDocument withAttributes(attributes);
  ExpectGrownTables(withAttributes);
  EXPECT_EQ(withAttributes.querySelectorAll("i").size(), kCount);
  EXPECT_TRUE(withAttributes.querySelector(Format("i[a{}]", kCount - 1)));
  const HtmlDocument withTags(tags);
  ExpectGrownTables(withTags);
  EXPECT_EQ(test::ChildCount(withTags.body()), kCount);
  EXPECT_TRUE(withTags.querySelector(Format("x-{}", kCount - 1)));
  const HtmlDocument withOneTag(oneTag);
  ExpectGrownTables(withOneTag);
  EXPECT_EQ(InternedNames(withOneTag).attributes, kCount);
  EXPECT_TRUE(withOneTag.querySelector("#after"));
}

// 2^bits distinct names of 4 * bits lower case letters of the same hash id in lexbor (lexbor_hash_make_id(), Jenkins
// one-at-a-time): each name takes one of the 2 blocks of each pair, which reach the same internal state of the hash
// from the one that the previous pairs reach (multicollisions of Joux, each pair found by a birthday search).
std::vector<std::string> CollidingNames(std::size_t bits) {
  static constexpr auto kBlockPairs = std::to_array<std::array<std::string_view, 2>>({
      {"puwa", "thae"},
      {"iima", "efae"},
      {"duoa", "hrae"},
      {"fewa", "jbae"},
      {"bkoa", "fhae"},
      {"yowa", "ubae"},
      {"nsta", "bnld"},
      {"ckma", "ghae"},
      {"xzoa", "tgae"},
      {"xnma", "taae"},
      {"ftoa", "jqae"},
      {"keaa", "kuta"},
      {"cpka", "gcae"},
      {"nnea", "jaae"},
  });
  EXPECT_LE(bits, kBlockPairs.size());
  std::vector<std::string> names(std::size_t{1} << bits);
  for (std::size_t idx = 0; idx < names.size(); ++idx) {
    for (std::size_t pair = 0; pair < bits; ++pair) {
      names[idx] += kBlockPairs[pair][(idx >> pair) & 1U];
    }
  }
  for (const std::string& name : names) {
    EXPECT_EQ(lexbor_hash_make_id(Chars(name), name.size()), lexbor_hash_make_id(Chars(names[0]), names[0].size()));
  }
  return names;
}

// Parses 'html', whose names of the same hash take 'bytesPerName' bytes each from its start: the parsing stops after
// its first chunk.
void ExpectParsingStoppedAfterFirstChunk(const std::string& html, std::size_t bytesPerName) {
  const HtmlDocument doc(html);
  EXPECT_TRUE(doc.querySelector("#before"));
  EXPECT_FALSE(doc.querySelector("#after"));
  const InternedNames interned(doc);
  EXPECT_GT(interned.tags + interned.attributes, kMaxCollidingNames);
  EXPECT_LE(interned.tags + interned.attributes, kParseChunkSize / bytesPerName);
}

TEST(HtmlNameTableTest, CollidingNamesStopTheParsing) {
  // Each name of the same hash is compared to all the previous ones, in the same bucket whatever the size of the table:
  // 1.1 s for these 900 KB of attributes, 1 s for the tags (quadratic). The parsing stops after the chunk whose names
  // fill a bucket beyond kMaxCollidingNames.
  const std::vector<std::string> names = CollidingNames(14);
  const std::size_t nameSize = names[0].size();
  std::string attributes = "<p id=before>x</p><i";
  std::string tags = "<p id=before>x</p>";
  for (const std::string& name : names) {
    FormatTo(attributes, " {}", name);
    FormatTo(tags, "<{}></{}>", name, name);
  }
  attributes += "><p id=after>y</p>";
  tags += "<p id=after>y</p>";
  ExpectParsingStoppedAfterFirstChunk(attributes, nameSize + 1);
  ExpectParsingStoppedAfterFirstChunk(tags, (2 * nameSize) + 5);
}

TEST(HtmlNameTableTest, BucketsHoldUpToTheLimitOfCollidingNames) {
  const std::vector<std::string> names = CollidingNames(7);
  for (const std::size_t count : {kMaxCollidingNames, kMaxCollidingNames + 1}) {
    std::string html = "<i";
    for (std::size_t idx = 0; idx < count; ++idx) {
      FormatTo(html, " {}", names[idx]);
    }
    // The next chunks are only parsed if the first one leaves no bucket beyond the limit.
    html += "></i>" + std::string(kParseChunkSize, 'x') + "<p id=after>";
    const HtmlDocument doc(html);
    EXPECT_EQ(test::AttributeCount(doc.querySelector("i")), count);
    EXPECT_EQ(static_cast<bool>(doc.querySelector("#after")), count == kMaxCollidingNames) << count;
  }
}

TEST(HtmlNameTableTest, CollidingNamesStopTheParsingOfAFragment) {
  HtmlDocument doc("<div id=target></div><p id=last></p>");
  std::string html = "<p id=before>x</p><i";
  for (const std::string& name : CollidingNames(10)) {
    FormatTo(html, " {}", name);
  }
  html += "><p id=after>y</p>";
  doc.querySelector("#target").setInnerHtml(html);
  EXPECT_TRUE(doc.querySelector("#target > #before"));
  EXPECT_FALSE(doc.querySelector("#after"));
  // Only the fragment stops.
  EXPECT_TRUE(doc.querySelector("#last"));
}

#if AERONET_WANT_MALLOC_OVERRIDES
TEST(HtmlNameTableTest, MemoryExhaustionKeepsTheTable) {
  HtmlDocument doc("<p></p>");
  InternNamesOfEachVariant(doc.body());
  lexbor_hash_t& tags = *LexborDocument(doc).tags;
  const std::size_t buckets = tags.table_size;
  test::FailNextMalloc();
  EXPECT_FALSE(GrowNameTable(tags, 4 * buckets));
  test::ResetToSysMalloc();
  EXPECT_EQ(tags.table_size, buckets);
  const InternedNames before(doc);
  InternNamesOfEachVariant(doc.body());
  EXPECT_EQ(InternedNames(doc), before);
}

TEST(HtmlNameTableTest, TableThatCouldNotGrowIsNotChecked) {
  // 128 names per bucket on average once the tables cannot grow (their initial size): beyond kMaxCollidingNames,
  // the parsing does not stop.
  static constexpr std::size_t kInitialBuckets = 128;
  static constexpr std::size_t kCount = 128 * kInitialBuckets;
  std::string html;
  for (std::size_t idx = 0; idx < kCount; ++idx) {
    FormatTo(html, "<x-{} a{}></x-{}>", idx, idx, idx);
  }
  test::FailCallocsOfAtLeast(kMaxNamesPerBucket * kInitialBuckets * sizeof(lexbor_hash_entry_t*));  // the growths
  const HtmlDocument doc(html);
  test::ResetToSysMalloc();
  EXPECT_EQ(LexborDocument(doc).tags->table_size, kInitialBuckets);
  EXPECT_EQ(LexborDocument(doc).attrs->table_size, kInitialBuckets);
  EXPECT_EQ(test::ChildCount(doc.body()), kCount);
  EXPECT_TRUE(doc.querySelector(Format("x-{}[a{}]", kCount - 1, kCount - 1)));
}
#endif

}  // namespace
}  // namespace aeronet::internal
