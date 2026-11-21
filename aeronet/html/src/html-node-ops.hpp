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

// Replaces the data of pNode, a text or comment node (DOM CharacterData.data setter): the views on its former data are
// invalidated. Throws std::bad_alloc on memory exhaustion, the data being unchanged.
void SetNodeData(lxb_dom_node* pNode, std::string_view data);

// Inserts pNode, out of the tree, before pReference among the children of pParent, after its last child when
// pReference is null.
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

// The tagName of the DOM of the element pElement: its qualified name, upper-cased for an HTML element, kept by lexbor
// after the first call. Throws std::bad_alloc on memory exhaustion.
[[nodiscard]] std::string_view DomTagName(lxb_dom_node* pElement);

// The memory that lexbor allocates for a node of a document (element, text, comment) or an attribute, without its data
// (measured: 140 to 190 bytes).
inline constexpr std::size_t kNodeMemory = 192;

// An estimate of the memory of the nodes parsed from HTML, per byte of HTML. Measured with lexbor 3.0.1: about 1 for
// text, 20 to 30 for most markup, up to 130 for the densest (a start tag every 3 bytes).
inline constexpr std::size_t kHtmlMemoryPerByte = 32;

// An estimate of the memory held by the nodes from pFirst to pEnd (excluded, null for after the last one), siblings,
// with their descendants (those of the template contents too): kNodeMemory for each node and attribute, plus their
// text and values. The nodes are elements, text, CDATA sections and comments, as in a parsed fragment.
[[nodiscard]] std::size_t NodesMemory(lxb_dom_node* pFirst, const lxb_dom_node* pEnd) noexcept;

// The source text of a classic script fetched with the body 'bytes' and the Content-Type 'contentType', decoded to
// UTF-8 as browsers decode it (HTML "fetch a classic script"): from the encoding of its byte order mark, else of the
// charset parameter of 'contentType', else of 'charset' (the charset attribute of the script), else
// 'documentEncoding', else UTF-8. Invalid sequences become U+FFFD. Returns 'bytes' itself, without its byte order mark,
// when it is valid UTF-8, else the text decoded into 'buffer', whose previous contents are discarded. Throws
// std::bad_alloc on memory exhaustion.
[[nodiscard]] std::string_view DecodeScript(std::string_view bytes, std::string_view contentType,
                                            std::string_view charset, std::string_view documentEncoding,
                                            RawChars& buffer);

// 'bytes' decoded as UTF-8, a byte order mark included (WHATWG Encoding "UTF-8 decode without BOM"): 'bytes' itself
// exactly when it is valid UTF-8, else the text decoded into 'buffer' (its previous contents discarded), invalid
// sequences becoming U+FFFD. Throws std::bad_alloc on memory exhaustion.
[[nodiscard]] std::string_view DecodeUtf8(std::string_view bytes, RawChars& buffer);

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
