#pragma once

#include <lexbor/core/base.h>
#include <lexbor/css/base.h>
#include <lexbor/css/parser.h>
#include <lexbor/css/selectors/selector.h>
#include <lexbor/css/selectors/selectors.h>
#include <lexbor/selectors/selectors.h>

#include <memory>
#include <string_view>

namespace aeronet::internal {

// lexbor functions only fail on memory exhaustion in the way aeronet calls them: throws std::bad_alloc unless status
// is LXB_STATUS_OK.
void CheckStatus(lxb_status_t status);

// Throws std::bad_alloc if the object that a lexbor function created is null, and returns it.
template <class T>
T* CheckCreated(T* pObject) {
  CheckStatus(pObject == nullptr ? LXB_STATUS_ERROR_MEMORY_ALLOCATION : LXB_STATUS_OK);
  return pObject;
}

struct CssParserDeleter {
  void operator()(lxb_css_parser_t* pParser) const noexcept { lxb_css_parser_destroy(pParser, true); }
};

struct CssMemoryDeleter {
  void operator()(lxb_css_memory_t* pMemory) const noexcept { lxb_css_memory_destroy(pMemory, true); }
};

struct CssSelectorsDeleter {
  void operator()(lxb_css_selectors_t* pSelectors) const noexcept { lxb_css_selectors_destroy(pSelectors, true); }
};

struct SelectorMatcherDeleter {
  void operator()(lxb_selectors_t* pMatcher) const noexcept { lxb_selectors_destroy(pMatcher, true); }
};

using CssParserPtr = std::unique_ptr<lxb_css_parser_t, CssParserDeleter>;
using CssMemoryPtr = std::unique_ptr<lxb_css_memory_t, CssMemoryDeleter>;
using CssSelectorsPtr = std::unique_ptr<lxb_css_selectors_t, CssSelectorsDeleter>;
using SelectorMatcherPtr = std::unique_ptr<lxb_selectors_t, SelectorMatcherDeleter>;

// A new CSS parser (with its own tokenizer).
CssParserPtr MakeCssParser();

// A new, small, CSS memory, for the selector lists of a parser.
CssMemoryPtr MakeCssMemory();

// A new parsing state of selectors, to give to a CSS parser (lxb_css_parser_selectors_set).
CssSelectorsPtr MakeCssSelectors();

// A new selector matching engine, each element being reported once even if it matches several selectors of a list.
SelectorMatcherPtr MakeSelectorMatcher();

// Parses 'selector' with pParser into its memory (lxb_css_parser_memory_set).
// Throws std::invalid_argument if it is not a valid selector list, std::bad_alloc on memory exhaustion.
lxb_css_selector_list_t* ParseSelectorList(lxb_css_parser_t* pParser, std::string_view selector);

}  // namespace aeronet::internal
