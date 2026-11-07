#include "aeronet/css-selector.hpp"

#include <lexbor/css/parser.h>
#include <lexbor/css/selectors/selector.h>

#include <cassert>
#include <string_view>
#include <utility>

#include "lexbor-utils.hpp"

namespace aeronet {

namespace {

// The parser only lives for the compilation: the list owns the memory it is allocated in.
lxb_css_selector_list_t* CompileSelectorList(std::string_view selector) {
  internal::CssParserPtr pParser = internal::MakeCssParser();
  internal::CssMemoryPtr pMemory = internal::MakeCssMemory();
  lxb_css_parser_memory_set(pParser.get(), pMemory.get());
  lxb_css_selector_list_t* pList = internal::ParseSelectorList(pParser.get(), selector);
  // From now on, the list owns the memory: lxb_css_selector_list_destroy_memory destroys it.
  [[maybe_unused]] const lxb_css_memory_t* pListMemory = pMemory.release();
  assert(pList->memory == pListMemory);
  return pList;
}

}  // namespace

CssSelector::CssSelector(std::string_view selector) : _pList(CompileSelectorList(selector)) {}

CssSelector& CssSelector::operator=(CssSelector&& other) noexcept {
  if (this != &other) {
    lxb_css_selector_list_destroy_memory(_pList);
    _pList = std::exchange(other._pList, nullptr);
  }
  return *this;
}

CssSelector::~CssSelector() { lxb_css_selector_list_destroy_memory(_pList); }

}  // namespace aeronet
