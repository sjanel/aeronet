#include <lexbor/core/base.h>
#include <lexbor/css/selectors/selector.h>
#include <lexbor/dom/interfaces/node.h>

#include <cassert>
#include <exception>
#include <string_view>

#include "aeronet/css-selector.hpp"
#include "aeronet/html-element.hpp"
#include "aeronet/vector.hpp"
#include "html-node-access.hpp"
#include "html-node-ops.hpp"
#include "html-query-engine.hpp"
#include "lexbor-utils.hpp"

namespace aeronet {

namespace {

using internal::HtmlNodeAccess;
using internal::HtmlQueryEngine;

lxb_status_t StoreFirstMatch(lxb_dom_node_t* pNode, [[maybe_unused]] lxb_css_selector_specificity_t specificity,
                             void* pFirst) noexcept {
  *static_cast<lxb_dom_node_t**>(pFirst) = pNode;
  return LXB_STATUS_STOP;
}

// An exception cannot cross the C frames of lexbor: the one thrown by the callback stops the search, and is rethrown
// after it. The callback also stops it by returning false.
struct VisitContext {
  internal::HtmlElementCallback callback;
  void* pCallback;
  std::exception_ptr exception;
};

lxb_status_t VisitMatch(lxb_dom_node_t* pNode, [[maybe_unused]] lxb_css_selector_specificity_t specificity,
                        void* pContext) noexcept {
  VisitContext& context = *static_cast<VisitContext*>(pContext);
  try {
    return context.callback(HtmlNodeAccess::Element(pNode), context.pCallback) ? LXB_STATUS_OK : LXB_STATUS_STOP;
  } catch (...) {
    context.exception = std::current_exception();
    return LXB_STATUS_STOP;
  }
}

HtmlElement FindFirst(HtmlQueryEngine::Query& query, lxb_dom_node_t* pRoot, const lxb_css_selector_list_t* pList) {
  lxb_dom_node_t* pFirst = nullptr;
  internal::CheckStatus(query.find(pRoot, pList, StoreFirstMatch, static_cast<void*>(&pFirst)));
  return HtmlNodeAccess::Element(pFirst);
}

void Visit(HtmlQueryEngine::Query& query, lxb_dom_node_t* pRoot, const lxb_css_selector_list_t* pList,
           internal::HtmlElementCallback callback, void* pCallback) {
  VisitContext context{callback, pCallback, {}};
  const lxb_status_t status = query.find(pRoot, pList, VisitMatch, &context);
  if (context.exception) {
    std::rethrow_exception(context.exception);
  }
  internal::CheckStatus(status);
}

bool AppendElement(HtmlElement element, void* pElements) {
  static_cast<vector<HtmlElement>*>(pElements)->push_back(element);
  return true;
}

lxb_dom_node_t* RootNode(const HtmlParentNode& node) noexcept {
  lxb_dom_node_t* pRoot = HtmlNodeAccess::Node(node);
  assert(pRoot != nullptr && "query on an invalid element or a moved-from document");
  return pRoot;
}

}  // namespace

bool internal::VisitMatches(lxb_dom_node_t* pRoot, std::string_view selector, HtmlElementCallback callback,
                            void* pCallback) {
  HtmlQueryEngine::Query query = HtmlQueryEngine::StartQuery(pRoot);
  const lxb_css_selector_list_t* pList = query.parse(selector);
  if (pList == nullptr) {
    return false;
  }
  Visit(query, pRoot, pList, callback, pCallback);
  return true;
}

HtmlElement HtmlParentNode::querySelector(std::string_view selector) const {
  lxb_dom_node_t* pRoot = RootNode(*this);
  HtmlQueryEngine::Query query = HtmlQueryEngine::StartQuery(pRoot);
  const lxb_css_selector_list_t* pList = query.parse(selector);
  if (pList == nullptr) {
    internal::ThrowInvalidSelector(selector);
  }
  return FindFirst(query, pRoot, pList);
}

HtmlElement HtmlParentNode::querySelector(const CssSelector& selector) const {
  lxb_dom_node_t* pRoot = RootNode(*this);
  HtmlQueryEngine::Query query = HtmlQueryEngine::StartQuery(pRoot);
  return FindFirst(query, pRoot, HtmlNodeAccess::List(selector));
}

vector<HtmlElement> HtmlParentNode::querySelectorAll(std::string_view selector) const {
  vector<HtmlElement> elements;
  forEachMatch(selector, AppendElement, &elements);
  return elements;
}

vector<HtmlElement> HtmlParentNode::querySelectorAll(const CssSelector& selector) const {
  vector<HtmlElement> elements;
  forEachMatch(selector, AppendElement, &elements);
  return elements;
}

void HtmlParentNode::forEachMatch(std::string_view selector, internal::HtmlElementCallback callback,
                                  void* pCallback) const {
  if (!internal::VisitMatches(RootNode(*this), selector, callback, pCallback)) {
    internal::ThrowInvalidSelector(selector);
  }
}

void HtmlParentNode::forEachMatch(const CssSelector& selector, internal::HtmlElementCallback callback,
                                  void* pCallback) const {
  lxb_dom_node_t* pRoot = RootNode(*this);
  HtmlQueryEngine::Query query = HtmlQueryEngine::StartQuery(pRoot);
  Visit(query, pRoot, HtmlNodeAccess::List(selector), callback, pCallback);
}

}  // namespace aeronet
