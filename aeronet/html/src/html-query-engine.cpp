#include "html-query-engine.hpp"

#include <lexbor/css/base.h>
#include <lexbor/css/parser.h>
#include <lexbor/css/selectors/selector.h>
#include <lexbor/dom/interfaces/document.h>
#include <lexbor/dom/interfaces/node.h>

#include <string_view>
#include <utility>

#include "lexbor-utils.hpp"

namespace aeronet::internal {

HtmlQueryEngine::Query::Query(HtmlQueryEngine& engine, Depth depth) : _engine(engine), _depth(depth) {
  if (depth == engine._levels.size()) {
    engine._levels.emplace_back(MakeSelectorMatcher(), CssMemoryPtr{});
  }
  _pMatcher = engine._levels[depth].matcher.get();
  // Last: the destructor, which releases the depth, does not run if the constructor throws.
  ++engine._depth;
}

const lxb_css_selector_list_t* HtmlQueryEngine::Query::parse(std::string_view selector) {
  if (!_engine._pParser) {
    CssSelectorsPtr pCssSelectors = MakeCssSelectors();
    _engine._pParser = MakeCssParser();
    lxb_css_parser_selectors_set(_engine._pParser.get(), pCssSelectors.get());
    _engine._pCssSelectors = std::move(pCssSelectors);
  }
  CssMemoryPtr& pMemory = _engine._levels[_depth].memory;
  if (pMemory) {
    // The list of the previous query of this depth is not used anymore.
    lxb_css_memory_clean(pMemory.get());
  } else {
    pMemory = MakeCssMemory();
  }
  lxb_css_parser_memory_set(_engine._pParser.get(), pMemory.get());
  return ParseSelectorList(_engine._pParser.get(), selector);
}

HtmlQueryEngine::Query HtmlQueryEngine::StartQuery(lxb_dom_node_t* pNode) {
  // The owner document of the document node is itself.
  lxb_dom_node_t* pDocumentNode = &pNode->owner_document->node;
  auto* pEngine = static_cast<HtmlQueryEngine*>(pDocumentNode->user);
  if (pEngine == nullptr) {
    pEngine = new HtmlQueryEngine();
    pDocumentNode->user = pEngine;
  }
  return {*pEngine, pEngine->_depth};
}

void HtmlQueryEngine::Destroy(lxb_dom_node_t* pDocumentNode) noexcept {
  delete static_cast<HtmlQueryEngine*>(pDocumentNode->user);
  pDocumentNode->user = nullptr;
}

}  // namespace aeronet::internal
