#pragma once

#include <lexbor/css/selectors/selector.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/selectors/selectors.h>

#include <string_view>
#include <type_traits>

#include "aeronet/amc-type-traits.hpp"
#include "aeronet/vector.hpp"
#include "lexbor-utils.hpp"

namespace aeronet::internal {

// The state shared by the selector queries of a document: the matching engines, and a CSS parser for the selectors
// given as text, with a memory reused by the queries. Created by the first query, stored in the user pointer of the
// document node, destroyed with the document.
// A forEach() callback can run other queries on the same document: each nesting depth has its own matching engine and
// selector memory, created on demand.
class HtmlQueryEngine {
 private:
  struct Level;

 public:
  using Depth = vector<Level>::size_type;

  // The search state of one query, released (made available to the next query of its depth) on destruction.
  class Query {
   public:
    Query(const Query&) = delete;
    Query& operator=(const Query&) = delete;
    Query(Query&&) = delete;
    Query& operator=(Query&&) = delete;

    ~Query() { --_engine._depth; }

    // Parses 'selector' into the memory of this query, valid until its end.
    // Throws std::invalid_argument if it is not a valid selector list.
    const lxb_css_selector_list_t* parse(std::string_view selector);

    // Calls cb for each element under pRoot matching pList, as lxb_selectors_find.
    lxb_status_t find(lxb_dom_node_t* pRoot, const lxb_css_selector_list_t* pList, lxb_selectors_cb_f cb, void* pCtx) {
      return lxb_selectors_find(_pMatcher, pRoot, pList, cb, pCtx);
    }

   private:
    friend class HtmlQueryEngine;

    Query(HtmlQueryEngine& engine, Depth depth);

    HtmlQueryEngine& _engine;
    lxb_selectors_t* _pMatcher;
    Depth _depth;
  };

  // Starts a query on the document of pNode (a document or element node), creating its engine on first use.
  static Query StartQuery(lxb_dom_node_t* pNode);

  // Destroys the engine of the document node pDocumentNode, if it has one.
  static void Destroy(lxb_dom_node_t* pDocumentNode) noexcept;

 private:
  struct Level {
    SelectorMatcherPtr matcher;
    CssMemoryPtr memory;  // created by the first query of the level with a selector given as text

    using trivially_relocatable = std::bool_constant<amc::is_trivially_relocatable_v<SelectorMatcherPtr> &&
                                                     amc::is_trivially_relocatable_v<CssMemoryPtr>>;
  };

  static_assert(amc::is_trivially_relocatable_v<Level>);

  vector<Level> _levels;
  CssParserPtr _pParser;           // created by the first query with a selector given as text
  CssSelectorsPtr _pCssSelectors;  // parsing state of _pParser, kept to not be created by each parse
  Depth _depth{};
};

}  // namespace aeronet::internal
