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
#include <lexbor/html/interfaces/document.h>
#include <lexbor/ns/const.h>
#include <lexbor/tag/const.h>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

#include "aeronet/cctype.hpp"
#include "aeronet/html-document.hpp"
#include "aeronet/html-element.hpp"
#include "aeronet/toupperlower.hpp"
#include "html-node-access.hpp"
#include "html-node-ops.hpp"
#include "html-parse.hpp"
#include "lexbor-utils.hpp"

#ifndef NDEBUG
#include "html-query-engine.hpp"
#endif

namespace aeronet {

namespace {

using internal::AsElement;
using internal::Chars;
using internal::CheckCreated;
using internal::Checked;
using internal::CheckStatus;
using internal::HtmlNodeAccess;

constexpr bool IsAsciiAlpha(char ch) noexcept { return islower(tolower(ch)); }

constexpr bool IsNonAscii(char ch) noexcept { return static_cast<unsigned char>(ch) >= 0x80; }

lxb_html_document_t* HtmlDocumentOf(lxb_dom_node_t* pNode) noexcept {
  // The HTML document starts with its DOM document.
  return reinterpret_cast<lxb_html_document_t*>(pNode->owner_document);
}

using internal::InsertionPoint;

InsertionPoint ResolvePosition(lxb_dom_node_t* pNode, HtmlInsertPosition position) {
  const InsertionPoint point = internal::AdjacentInsertionPoint(pNode, position);
  if (!internal::CanInsertAdjacent(point)) {
    throw std::invalid_argument(
        "HtmlInsertPosition::BeforeBegin and AfterEnd need an element with a parent, not the root");
  }
  return point;
}

void Insert(const InsertionPoint& point, lxb_dom_node_t* pNode) noexcept {
  internal::InsertNode(point.pParent, pNode, point.pReference);
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
  return text.empty() ? nullptr : internal::NewTextNode(pNode, text);
}

// The tree must not change during a search: a forEach() callback walks it.
void AssertTreeCanChange([[maybe_unused]] lxb_dom_node_t* pNode) noexcept {
  assert(!internal::HtmlQueryEngine::Searching(pNode) && "tree modified by a forEach() callback");
}

}  // namespace

// DOM "valid element local name". Each byte of a UTF-8 sequence is non-ASCII, as the code point it encodes.
bool HtmlDocument::IsValidElementName(std::string_view localName) noexcept {
  if (localName.empty()) {
    return false;
  }
  if (IsAsciiAlpha(localName.front())) {
    return std::ranges::none_of(localName,
                                [](char ch) { return IsAsciiWhitespace(ch) || ch == '\0' || ch == '/' || ch == '>'; });
  }
  if (localName.front() != ':' && localName.front() != '_' && !IsNonAscii(localName.front())) {
    return false;
  }
  return std::ranges::all_of(localName.substr(1), [](char ch) {
    return IsAsciiAlpha(ch) || isdigit(ch) || ch == '-' || ch == '.' || ch == ':' || ch == '_' || IsNonAscii(ch);
  });
}

// DOM "valid attribute local name".
bool HtmlElement::IsValidAttributeName(std::string_view name) noexcept {
  return !name.empty() && std::ranges::none_of(name, [](char ch) {
    return IsAsciiWhitespace(ch) || ch == '\0' || ch == '/' || ch == '=' || ch == '>';
  });
}

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
  // The fragment parsing algorithm of insertAdjacentHTML takes a <body> for an <html> context, and for a document
  // fragment (the contents of a template).
  const lxb_dom_node_t& context = *point.pParent;
  const bool bodyContext =
      context.type != LXB_DOM_NODE_TYPE_ELEMENT || (context.local_name == LXB_TAG_HTML && context.ns == LXB_NS_HTML);
  InsertFragment(
      internal::ParseHtmlFragment(HtmlDocumentOf(_pNode),
                                  bodyContext ? static_cast<std::uintptr_t>(LXB_TAG_BODY) : context.local_name,
                                  bodyContext ? static_cast<std::uintptr_t>(LXB_NS_HTML) : context.ns, html),
      point);
}

void HtmlElement::insertAdjacentElement(HtmlInsertPosition position, HtmlElement element) {
  AssertTreeCanChange(Checked(_pNode));
  lxb_dom_node_t* pNode = Checked(element._pNode);
  if (pNode->owner_document != _pNode->owner_document) {
    throw std::invalid_argument("HtmlElement inserted into another document");
  }
  InsertionPoint point = ResolvePosition(_pNode, position);
  if (internal::IsHostIncludingInclusiveAncestor(pNode, point.pParent)) {
    throw std::invalid_argument("HtmlElement inserted into itself or one of its descendants");
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
