#pragma once

#include <lexbor/css/selectors/selector.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/html/interfaces/template_element.h>
#include <lexbor/ns/const.h>
#include <lexbor/tag/const.h>

#include <cassert>
#include <string_view>

#include "aeronet/css-selector.hpp"
#include "aeronet/html-document.hpp"
#include "aeronet/html-element.hpp"

namespace aeronet::internal {

// Access of the implementation to the lexbor objects behind the public classes.
struct HtmlNodeAccess {
  // A handle on pNode, an element node (or nullptr for an invalid element).
  static HtmlElement Element(lxb_dom_node_t* pNode) noexcept { return HtmlElement(pNode); }

  static lxb_dom_node_t* Node(const HtmlParentNode& node) noexcept { return node._pNode; }

  // The compiled list of a selector passed to a query: never an empty one.
  static const lxb_css_selector_list_t* List(const CssSelector& selector) noexcept {
    assert(!selector.empty() && "query with an empty (default constructed or moved-from) CssSelector");
    return selector._pList;
  }

  // 'html' parsed as HtmlDocument(html, contentType) does, with scripting enabled: the contents of a <noscript> element
  // are text, as in the browsers that run scripts.
  static HtmlDocument ParseWithScripting(std::string_view html, std::string_view contentType) {
    return {html, contentType, true};
  }
};

// pNode, the node of an HtmlElement on which a method is called: valid.
inline lxb_dom_node_t* Checked(lxb_dom_node_t* pNode) noexcept {
  assert(pNode != nullptr && "method called on an invalid HtmlElement");
  return pNode;
}

// The element interfaces start with their node.
inline lxb_dom_element_t* AsElement(lxb_dom_node_t* pNode) noexcept {
  return reinterpret_cast<lxb_dom_element_t*>(Checked(pNode));
}

// Whether pNode is an HTML <template> element, whose contents are a document fragment created with it.
inline bool IsHtmlTemplate(const lxb_dom_node_t* pNode) noexcept {
  return pNode->local_name == LXB_TAG_TEMPLATE && pNode->ns == LXB_NS_HTML;
}

// The node holding the children that innerHTML reads and writes for the element pNode: the contents of a <template>
// (a document fragment, created with the element), the element itself otherwise.
inline lxb_dom_node_t* ChildrenParent(lxb_dom_node_t* pNode) noexcept {
  if (IsHtmlTemplate(pNode)) {
    return &reinterpret_cast<lxb_html_template_element_t*>(pNode)->content->node;
  }
  return pNode;
}

}  // namespace aeronet::internal
