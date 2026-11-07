#pragma once

#include <lexbor/css/selectors/selector.h>
#include <lexbor/dom/interfaces/node.h>

#include "aeronet/css-selector.hpp"
#include "aeronet/html-element.hpp"

namespace aeronet::internal {

// Access of the implementation to the lexbor objects behind the public classes.
struct HtmlNodeAccess {
  // A handle on pNode, an element node (or nullptr for an invalid element).
  static HtmlElement Element(lxb_dom_node_t* pNode) noexcept { return HtmlElement(pNode); }

  static lxb_dom_node_t* Node(const HtmlParentNode& node) noexcept { return node._pNode; }

  static const lxb_css_selector_list_t* List(const CssSelector& selector) noexcept { return selector._pList; }
};

}  // namespace aeronet::internal
