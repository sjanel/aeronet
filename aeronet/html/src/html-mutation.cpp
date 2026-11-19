// The DOM mutations of HtmlElement and HtmlDocument.
// Nodes taken out of the tree and removed attributes are unlinked, never destroyed: the handles and views on them stay
// valid as long as their document lives (lexbor destroys them in lxb_html_element_inner_html_set,
// lxb_dom_node_replace_all and lxb_dom_element_remove_attribute, which this file does not use).

#include <lexbor/core/base.h>
#include <lexbor/core/types.h>
#include <lexbor/dom/interfaces/attr.h>
#include <lexbor/dom/interfaces/document.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/dom/interfaces/text.h>
#include <lexbor/html/interfaces/document.h>
#include <lexbor/ns/const.h>
#include <lexbor/tag/const.h>

#include <algorithm>
#include <cassert>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

#include "aeronet/cctype.hpp"
#include "aeronet/html-document.hpp"
#include "aeronet/html-element.hpp"
#include "aeronet/toupperlower.hpp"
#include "html-node-access.hpp"
#include "html-parse.hpp"
#include "lexbor-utils.hpp"

#ifndef NDEBUG
#include "html-query-engine.hpp"
#endif

namespace aeronet {

namespace {

using internal::AsElement;
using internal::CheckCreated;
using internal::Checked;
using internal::CheckStatus;
using internal::HtmlNodeAccess;

const lxb_char_t* Chars(std::string_view str) noexcept { return reinterpret_cast<const lxb_char_t*>(str.data()); }

// The ASCII whitespace of the DOM: no vertical tab.
constexpr bool IsAsciiWhitespace(char ch) noexcept {
  return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\f' || ch == '\r';
}

constexpr bool IsAsciiAlpha(char ch) noexcept { return islower(tolower(ch)); }

constexpr bool IsNonAscii(char ch) noexcept { return static_cast<unsigned char>(ch) >= 0x80; }

// DOM "valid attribute local name".
constexpr bool IsValidAttributeName(std::string_view name) noexcept {
  return !name.empty() && std::ranges::none_of(name, [](char ch) {
    return IsAsciiWhitespace(ch) || ch == '\0' || ch == '/' || ch == '=' || ch == '>';
  });
}

// DOM "valid element local name". Each byte of a UTF-8 sequence is non-ASCII, as the code point it encodes.
constexpr bool IsValidElementName(std::string_view name) noexcept {
  if (name.empty()) {
    return false;
  }
  if (IsAsciiAlpha(name.front())) {
    return std::ranges::none_of(name,
                                [](char ch) { return IsAsciiWhitespace(ch) || ch == '\0' || ch == '/' || ch == '>'; });
  }
  if (name.front() != ':' && name.front() != '_' && !IsNonAscii(name.front())) {
    return false;
  }
  return std::ranges::all_of(name.substr(1), [](char ch) {
    return IsAsciiAlpha(ch) || isdigit(ch) || ch == '-' || ch == '.' || ch == ':' || ch == '_' || IsNonAscii(ch);
  });
}

lxb_html_document_t* HtmlDocumentOf(lxb_dom_node_t* pNode) noexcept {
  // The HTML document starts with its DOM document.
  return reinterpret_cast<lxb_html_document_t*>(pNode->owner_document);
}

// Where the nodes inserted at a position go: before pReference among the children of pParent, or after its last child
// if pReference is null.
struct InsertionPoint {
  lxb_dom_node_t* pParent;
  lxb_dom_node_t* pReference;
};

// The parent element of an element, for an insertion beside it. The elements of a template contents, of a document
// fragment parent, cannot be reached.
lxb_dom_node_t* SiblingParent(lxb_dom_node_t* pNode) {
  lxb_dom_node_t* pParent = pNode->parent;
  if (pParent == nullptr || pParent->type != LXB_DOM_NODE_TYPE_ELEMENT) {
    throw std::invalid_argument("HtmlInsertPosition::BeforeBegin and AfterEnd need an element with a parent element");
  }
  return pParent;
}

InsertionPoint ResolvePosition(lxb_dom_node_t* pNode, HtmlInsertPosition position) {
  switch (position) {
    case HtmlInsertPosition::BeforeBegin:
      return {SiblingParent(pNode), pNode};
    case HtmlInsertPosition::AfterBegin:
      return {pNode, pNode->first_child};
    case HtmlInsertPosition::BeforeEnd:
      return {pNode, nullptr};
    default:
      assert(position == HtmlInsertPosition::AfterEnd);
      return {SiblingParent(pNode), pNode->next};
  }
}

void Insert(const InsertionPoint& point, lxb_dom_node_t* pNode) noexcept {
  if (point.pReference != nullptr) {
    lxb_dom_node_insert_before(point.pReference, pNode);
  } else {
    lxb_dom_node_insert_child(point.pParent, pNode);
  }
}

// Moves the children of pFragmentRoot, the root of a parsed fragment, at 'point', then destroys the root.
void InsertFragment(lxb_dom_node_t* pFragmentRoot, const InsertionPoint& point) noexcept {
  while (lxb_dom_node_t* pChild = pFragmentRoot->first_child) {
    lxb_dom_node_remove(pChild);
    Insert(point, pChild);
  }
  lxb_dom_node_destroy(pFragmentRoot);
}

void RemoveChildren(lxb_dom_node_t* pParent) noexcept {
  while (pParent->first_child != nullptr) {
    lxb_dom_node_remove(pParent->first_child);
  }
}

// A text node of 'text' out of the tree, or null if 'text' is empty.
lxb_dom_node_t* CreateTextNode(lxb_dom_node_t* pNode, std::string_view text) {
  if (text.empty()) {
    return nullptr;
  }
  return &CheckCreated(lxb_dom_document_create_text_node(pNode->owner_document, Chars(text), text.size()))
              ->char_data.node;
}

// The tree must not change during a search: a forEach() callback walks it.
void AssertTreeCanChange([[maybe_unused]] lxb_dom_node_t* pNode) noexcept {
  assert(!internal::HtmlQueryEngine::Searching(pNode) && "tree modified by a forEach() callback");
}

}  // namespace

HtmlElement HtmlDocument::createElement(std::string_view localName) {
  if (!IsValidElementName(localName)) {
    throw std::invalid_argument(std::string("Invalid element name '").append(localName).append("'"));
  }
  // An HTML document (the owner of the document node is itself): the name is lower-cased, in the HTML namespace.
  lxb_dom_element_t* pElement = CheckCreated(
      lxb_dom_document_create_element(_pNode->owner_document, Chars(localName), localName.size(), nullptr));
  return HtmlNodeAccess::Element(&pElement->node);
}

void HtmlElement::setAttribute(std::string_view name, std::string_view value) {
  if (!IsValidAttributeName(name)) {
    throw std::invalid_argument(std::string("Invalid attribute name '").append(name).append("'"));
  }
  lxb_dom_element_t* pElement = AsElement(_pNode);
  lxb_dom_attr_t* pAttr = lxb_dom_element_attr_by_name(pElement, Chars(name), name.size());
  if (pAttr != nullptr) {
    // On failure, the value is empty.
    CheckStatus(lxb_dom_attr_set_value(pAttr, Chars(value), value.size()));
    return;
  }
  // Not lxb_dom_element_set_attribute: it destroys the new attribute if its append steps fail, while it is in the
  // attribute list of the element.
  pAttr = CheckCreated(lxb_dom_attr_interface_create(pElement->node.owner_document));
  pAttr->node.ns = pElement->node.ns;
  lxb_status_t status = lxb_dom_attr_set_name(pAttr, Chars(name), name.size(), pElement->node.ns == LXB_NS_HTML);
  if (status == LXB_STATUS_OK) {
    status = lxb_dom_attr_set_value(pAttr, Chars(value), value.size());
  }
  if (status != LXB_STATUS_OK) {
    lxb_dom_attr_interface_destroy(pAttr);
    throw std::bad_alloc();
  }
  // On failure of the append steps, the attribute stays in the element.
  CheckStatus(lxb_dom_element_attr_append(pElement, pAttr));
}

void HtmlElement::removeAttribute(std::string_view name) {
  lxb_dom_element_t* pElement = AsElement(_pNode);
  lxb_dom_attr_t* pAttr = lxb_dom_element_attr_by_name(pElement, Chars(name), name.size());
  if (pAttr != nullptr) {
    CheckStatus(lxb_dom_element_attr_remove(pElement, pAttr));
  }
}

void HtmlElement::setTextContent(std::string_view text) {
  AssertTreeCanChange(Checked(_pNode));
  lxb_dom_node_t* pText = CreateTextNode(_pNode, text);
  RemoveChildren(_pNode);
  if (pText != nullptr) {
    lxb_dom_node_insert_child(_pNode, pText);
  }
}

void HtmlElement::setInnerHtml(std::string_view html) {
  AssertTreeCanChange(Checked(_pNode));
  lxb_dom_node_t* pFragment = internal::ParseHtmlFragment(HtmlDocumentOf(_pNode), _pNode->local_name, _pNode->ns, html);
  lxb_dom_node_t* pParent = internal::ChildrenParent(_pNode);
  RemoveChildren(pParent);
  InsertFragment(pFragment, {pParent, nullptr});
}

void HtmlElement::insertAdjacentHtml(HtmlInsertPosition position, std::string_view html) {
  AssertTreeCanChange(Checked(_pNode));
  const InsertionPoint point = ResolvePosition(_pNode, position);
  // The fragment parsing algorithm of insertAdjacentHTML takes a <body> for an <html> context.
  const lxb_dom_node_t& context = *point.pParent;
  const bool rootContext = context.local_name == LXB_TAG_HTML && context.ns == LXB_NS_HTML;
  InsertFragment(internal::ParseHtmlFragment(HtmlDocumentOf(_pNode), rootContext ? LXB_TAG_BODY : context.local_name,
                                             rootContext ? LXB_NS_HTML : context.ns, html),
                 point);
}

void HtmlElement::insertAdjacentElement(HtmlInsertPosition position, HtmlElement element) {
  AssertTreeCanChange(Checked(_pNode));
  lxb_dom_node_t* pNode = Checked(element._pNode);
  if (pNode->owner_document != _pNode->owner_document) {
    throw std::invalid_argument("HtmlElement inserted into another document");
  }
  InsertionPoint point = ResolvePosition(_pNode, position);
  for (const lxb_dom_node_t* pAncestor = point.pParent; pAncestor != nullptr; pAncestor = pAncestor->parent) {
    if (pAncestor == pNode) {
      throw std::invalid_argument("HtmlElement inserted into itself or one of its descendants");
    }
  }
  // Inserted before itself: before its next sibling once removed.
  if (point.pReference == pNode) {
    point.pReference = pNode->next;
  }
  lxb_dom_node_remove(pNode);
  Insert(point, pNode);
}

void HtmlElement::insertAdjacentText(HtmlInsertPosition position, std::string_view text) {
  AssertTreeCanChange(Checked(_pNode));
  const InsertionPoint point = ResolvePosition(_pNode, position);
  lxb_dom_node_t* pText = CreateTextNode(_pNode, text);
  if (pText != nullptr) {
    Insert(point, pText);
  }
}

void HtmlElement::remove() noexcept {
  AssertTreeCanChange(Checked(_pNode));
  lxb_dom_node_remove(_pNode);
}

}  // namespace aeronet
