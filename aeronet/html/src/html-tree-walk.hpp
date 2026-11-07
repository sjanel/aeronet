#pragma once

#include <lexbor/dom/interfaces/node.h>

namespace aeronet::internal {

// Calls fn(lxb_dom_node_t*) for each descendant of pRoot in document order (pre-order depth-first), until it returns
// false.
template <class Fn>
void WalkDescendants(lxb_dom_node_t* pRoot, Fn fn) {
  for (lxb_dom_node_t* pNode = pRoot->first_child; pNode != nullptr && fn(pNode);) {
    if (pNode->first_child != nullptr) {
      pNode = pNode->first_child;
      continue;
    }
    while (pNode != pRoot && pNode->next == nullptr) {
      pNode = pNode->parent;
    }
    pNode = pNode == pRoot ? nullptr : pNode->next;
  }
}

// Text and CDATA section nodes (the Text nodes of the DOM).
inline bool IsTextNode(const lxb_dom_node_t* pNode) noexcept {
  return pNode->type == LXB_DOM_NODE_TYPE_TEXT || pNode->type == LXB_DOM_NODE_TYPE_CDATA_SECTION;
}

}  // namespace aeronet::internal
