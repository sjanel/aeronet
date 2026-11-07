#include "lexbor-utils.hpp"

#include <lexbor/core/base.h>
#include <lexbor/core/types.h>
#include <lexbor/css/base.h>
#include <lexbor/css/log.h>
#include <lexbor/css/parser.h>
#include <lexbor/css/selectors/selector.h>
#include <lexbor/css/selectors/selectors.h>
#include <lexbor/selectors/selectors.h>

#include <cstddef>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>

namespace aeronet::internal {

void CheckStatus(lxb_status_t status) {
  if (status != LXB_STATUS_OK) {
    throw std::bad_alloc();
  }
}

CssParserPtr MakeCssParser() {
  CssParserPtr pParser(CheckCreated(lxb_css_parser_create()));
  CheckStatus(lxb_css_parser_init(pParser.get(), nullptr));
  return pParser;
}

CssMemoryPtr MakeCssMemory() {
  CssMemoryPtr pMemory(CheckCreated(lxb_css_memory_create()));
  // The minimum: a selector list rarely needs more than a few dozen objects, the memory grows if needed.
  static constexpr std::size_t kPreparedObjects = 64;
  CheckStatus(lxb_css_memory_init(pMemory.get(), kPreparedObjects));
  return pMemory;
}

CssSelectorsPtr MakeCssSelectors() {
  CssSelectorsPtr pSelectors(CheckCreated(lxb_css_selectors_create()));
  CheckStatus(lxb_css_selectors_init(pSelectors.get()));
  return pSelectors;
}

SelectorMatcherPtr MakeSelectorMatcher() {
  SelectorMatcherPtr pMatcher(CheckCreated(lxb_selectors_create()));
  CheckStatus(lxb_selectors_init(pMatcher.get()));
  lxb_selectors_opt_set(pMatcher.get(), LXB_SELECTORS_OPT_MATCH_FIRST);
  return pMatcher;
}

lxb_css_selector_list_t* ParseSelectorList(lxb_css_parser_t* pParser, std::string_view selector) {
  lxb_css_selector_list_t* pList =
      lxb_css_selectors_parse(pParser, reinterpret_cast<const lxb_char_t*>(selector.data()), selector.size());
  if (pList == nullptr) {
    // The parser logs each syntax error (its log is cleaned by each parse). Without any, the list is missing because
    // an allocation failed, which lexbor does not report in the status of the parser.
    if (lxb_css_log_length(pParser->log) == 0) {
      throw std::bad_alloc();
    }
    throw std::invalid_argument(std::string("Invalid CSS selector '").append(selector).append("'"));
  }
  return pList;
}

}  // namespace aeronet::internal
