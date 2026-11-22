#pragma once

#include <lexbor/url/url.h>

#include <cstddef>
#include <initializer_list>
#include <string_view>

#include "aeronet/html-element.hpp"
#include "aeronet/raw-chars.hpp"

struct lxb_dom_node;

namespace aeronet::internal {

// The node operations of the DOM that HtmlElement does not expose (text and comment nodes, insertion of any node), on
// the lexbor nodes of an HtmlDocument: the JavaScript bindings of aeronet_web call them, as aeronet_html is the only
// library that links lexbor. Like the mutations of HtmlElement, they never destroy a node before its document.

// A new text node of 'text' (can be empty) of the document of pNode, out of the tree. Throws std::bad_alloc on memory
// exhaustion.
lxb_dom_node* NewTextNode(lxb_dom_node* pNode, std::string_view text);

// A new comment node of 'text' (can be empty) of the document of pNode, out of the tree. Throws std::bad_alloc on
// memory exhaustion.
lxb_dom_node* NewCommentNode(lxb_dom_node* pNode, std::string_view text);

// A new document fragment of the document of pNode. Throws std::bad_alloc on memory exhaustion.
lxb_dom_node* NewDocumentFragment(lxb_dom_node* pNode);

// A new element of the document of pNode, out of the tree, of the namespace 'namespaceUri' (empty for none) and the
// qualified name 'qualifiedName' ("svg" or "xlink:href"), whose local name and prefix are valid element names (see
// HtmlDocument::IsValidElementName()): DOM createElementNS(). lexbor lower-cases the local name. Throws std::bad_alloc
// on memory exhaustion.
lxb_dom_node* NewElementNs(lxb_dom_node* pNode, std::string_view namespaceUri, std::string_view qualifiedName);

// The namespace URI of the element pElement ("http://www.w3.org/1999/xhtml"), empty for none.
[[nodiscard]] std::string_view NamespaceUri(const lxb_dom_node* pElement) noexcept;

// Called by CloneNode() with each node and its copy, and the data given to it.
using ClonedFn = void (*)(const lxb_dom_node* pSource, lxb_dom_node* pCopy, void* pData);

// A copy of pNode (an element, a text or comment node, or a document fragment) in its document, out of the tree, with
// its attributes and the qualified name of an element (its prefix, the case of an SVG name), and for 'deep' its
// descendants and the contents of its templates (DOM "clone a node"). The copies have no user pointer: lexbor would
// copy that of each original, the JavaScript wrapper of the page. 'onCloned' (can be null) is called with 'pData' for
// each copy (the cloning steps of the DOM). Iterative, as scripts can nest elements at any depth. Throws
// std::bad_alloc on memory exhaustion: the copies made stay in the document, out of the tree.
lxb_dom_node* CloneNode(lxb_dom_node* pNode, bool deep, ClonedFn onCloned = nullptr, void* pData = nullptr);

// Replaces the data of pNode, a text or comment node (DOM CharacterData.data setter): the views on its former data are
// invalidated. Throws std::bad_alloc on memory exhaustion, the data being unchanged.
void SetNodeData(lxb_dom_node* pNode, std::string_view data);

// Whether pNode is pParent or one of its ancestors, the template whose contents hold pParent included (DOM
// "host-including inclusive ancestor"): pNode cannot be inserted into pParent, as the tree would contain itself and its
// walks (CloneNode(), NodesMemory(), serializations) would never end.
[[nodiscard]] bool IsHostIncludingInclusiveAncestor(const lxb_dom_node* pNode, const lxb_dom_node* pParent) noexcept;

// Inserts pNode, out of the tree, before pReference among the children of pParent, after its last child when
// pReference is null. pNode must not be a host-including inclusive ancestor of pParent.
void InsertNode(lxb_dom_node* pParent, lxb_dom_node* pNode, lxb_dom_node* pReference) noexcept;

// Takes pNode, with its descendants, out of the tree.
void RemoveNode(lxb_dom_node* pNode) noexcept;

// Where nodes are inserted: before pReference among the children of pParent, after its last child when pReference is
// null.
struct InsertionPoint {
  lxb_dom_node* pParent;
  lxb_dom_node* pReference;
};

// The insertion point of 'position' of the element pElement (DOM insertAdjacent*): beside it, among the children of its
// parent, for BeforeBegin and AfterEnd (a null parent when it has none), in it for AfterBegin and BeforeEnd.
[[nodiscard]] InsertionPoint AdjacentInsertionPoint(lxb_dom_node* pElement, HtmlInsertPosition position) noexcept;

// Whether the insertAdjacent*() methods of HtmlElement insert at 'point', an AdjacentInsertionPoint(): in an element or
// a document fragment (the contents of a template), not beside an element without parent nor beside the root element.
// They throw std::invalid_argument otherwise.
[[nodiscard]] bool CanInsertAdjacent(const InsertionPoint& point) noexcept;

// Calls callback(element, pCallback) for each element under pRoot (an element or document node) matching 'selector',
// in document order, while it returns true: HtmlParentNode::forEach() of a selector given as text, whose invalid
// selector is a status instead of an exception. Returns false, without searching, when 'selector' is not a valid
// selector list. An exception thrown by the callback stops the search and is propagated. Throws std::bad_alloc on
// memory exhaustion.
[[nodiscard]] bool VisitMatches(lxb_dom_node* pRoot, std::string_view selector, HtmlElementCallback callback,
                                void* pCallback);

// The result of MatchSelector().
struct SelectorMatch {
  lxb_dom_node* pElement;  // the element found, null if none
  bool valid;              // false when the selector is not a valid selector list (nothing searched)
};

// The first element among pElement and, for 'ancestors', its ancestor elements from the nearest, that matches
// 'selector': DOM Element.matches() and closest(). Throws std::bad_alloc on memory exhaustion.
[[nodiscard]] SelectorMatch MatchSelector(lxb_dom_node* pElement, std::string_view selector, bool ancestors);

// The tagName of the DOM of the element pElement: its qualified name, upper-cased for an HTML element, kept by lexbor
// after the first call. Throws std::bad_alloc on memory exhaustion.
[[nodiscard]] std::string_view DomTagName(lxb_dom_node* pElement);

// The memory that lexbor allocates for a node of a document (element, text, comment) or an attribute, without its data
// (measured: 140 to 190 bytes).
inline constexpr std::size_t kNodeMemory = 192;

// An estimate of the memory of the nodes parsed from HTML, per byte of HTML. Measured with lexbor 3.0.1: about 1 for
// text, 20 to 30 for most markup, up to 130 for the densest (a start tag every 3 bytes).
inline constexpr std::size_t kHtmlMemoryPerByte = 32;

// An estimate of the memory held by pNode alone (an element, text, CDATA section or comment): kNodeMemory, plus its
// text, or its attributes (kNodeMemory each, plus their values).
[[nodiscard]] std::size_t NodeMemory(const lxb_dom_node* pNode) noexcept;

// An estimate of the memory held by the nodes from pFirst to pEnd (excluded, null for after the last one), siblings,
// with their descendants (those of the template contents too): kNodeMemory for each node and attribute, plus their
// text and values. The nodes are elements, text, CDATA sections and comments, as in a parsed fragment.
[[nodiscard]] std::size_t NodesMemory(lxb_dom_node* pFirst, const lxb_dom_node* pEnd) noexcept;

// Resolves URLs as browsers resolve those of a page (WHATWG URL Standard, URL parser of lexbor): "../a?b" against
// "https://example.com/x/y" is "https://example.com/a?b". Its parser and the last base it parsed are kept: the URLs of
// a page are resolved against the same base, parsed again only when it changes.
class UrlResolver {
 public:
  // Throws std::bad_alloc on memory exhaustion.
  UrlResolver();

  UrlResolver(const UrlResolver&) = delete;
  UrlResolver& operator=(const UrlResolver&) = delete;
  UrlResolver(UrlResolver&&) = delete;
  UrlResolver& operator=(UrlResolver&&) = delete;

  ~UrlResolver();

  // The serialization of the URL parsed from 'input' against the URL 'base' ('base' can be empty for an absolute
  // 'input'), null-terminated at size(), valid until the next call. Empty when 'input' (or 'base') is not a valid URL,
  // and on most allocation failures of the parsing (lexbor does not tell them apart). Throws std::bad_alloc on the
  // other allocation failures.
  [[nodiscard]] std::string_view resolve(std::string_view base, std::string_view input);

  // resolve() of the concatenation of 'inputParts', built in a buffer of this resolver (no allocation once it is large
  // enough): the parts must not view it.
  [[nodiscard]] std::string_view resolve(std::string_view base, std::initializer_list<std::string_view> inputParts);

  // The input of the last resolve(base, inputParts): the concatenation of its parts.
  [[nodiscard]] std::string_view concatenatedInput() const noexcept { return _input; }

 private:
  RawChars _base;   // the text of _pBase: empty without base, or after a base that could not be parsed
  RawChars _url;    // the last serialization
  RawChars _input;  // the last input concatenated by resolve(base, inputParts)
  lxb_url_t* _pBase{};
  lxb_url_parser_t _parser;
};

}  // namespace aeronet::internal
