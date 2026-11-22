#pragma once

#include <lexbor/core/base.h>
#include <lexbor/core/types.h>
#include <lexbor/css/base.h>
#include <lexbor/css/parser.h>
#include <lexbor/css/selectors/selector.h>
#include <lexbor/css/selectors/selectors.h>
#include <lexbor/css/syntax/tokenizer.h>
#include <lexbor/selectors/selectors.h>

#include <cstddef>
#include <exception>
#include <memory>
#include <string_view>

namespace aeronet::internal {

// The bytes of 'str', given to lexbor: never null, even for an empty view (as an absent attribute value), as lexbor
// passes them to memcpy (undefined with a null pointer, even for 0 bytes) and some of its functions refuse null.
inline const lxb_char_t* Chars(std::string_view str) noexcept {
  return reinterpret_cast<const lxb_char_t*>(str.empty() ? "" : str.data());
}

// lexbor functions only fail on memory exhaustion in the way aeronet calls them: throws std::bad_alloc unless status
// is LXB_STATUS_OK.
void CheckStatus(lxb_status_t status);

// Appends the chunks of a lexbor serialization to a string. An exception cannot cross the C frames of lexbor: the one
// thrown by an append stops the serialization, and finish() rethrows it. Buffer is std::string or RawChars.
template <class Buffer>
class SerializationAppender {
 public:
  explicit SerializationAppender(Buffer& out) noexcept : _pOut(&out) {}

  // The callback of the lexbor serialization functions (lexbor_serialize_cb_f), given the appender as context.
  static lxb_status_t Append(const lxb_char_t* pData, std::size_t size, void* pAppender) noexcept {
    SerializationAppender& appender = *static_cast<SerializationAppender*>(pAppender);
    try {
      appender._pOut->append(reinterpret_cast<const char*>(pData), size);
    } catch (...) {
      appender._exception = std::current_exception();
      return LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    }
    return LXB_STATUS_OK;
  }

  // Rethrows the exception of an append, else checks the status of the serialization (see CheckStatus).
  void finish(lxb_status_t status) {
    if (_exception) {
      std::rethrow_exception(_exception);
    }
    CheckStatus(status);
  }

 private:
  std::exception_ptr _exception;
  Buffer* _pOut;
};

// Throws std::bad_alloc if the object that a lexbor function created is null, and returns it.
template <class T>
T* CheckCreated(T* pObject) {
  CheckStatus(pObject == nullptr ? LXB_STATUS_ERROR_MEMORY_ALLOCATION : LXB_STATUS_OK);
  return pObject;
}

struct CssTokenizerDeleter {
  void operator()(lxb_css_syntax_tokenizer_t* pTokenizer) const noexcept {
    lxb_css_syntax_tokenizer_destroy(pTokenizer);
  }
};

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

using CssTokenizerPtr = std::unique_ptr<lxb_css_syntax_tokenizer_t, CssTokenizerDeleter>;
using CssParserPtr = std::unique_ptr<lxb_css_parser_t, CssParserDeleter>;
using CssMemoryPtr = std::unique_ptr<lxb_css_memory_t, CssMemoryDeleter>;
using CssSelectorsPtr = std::unique_ptr<lxb_css_selectors_t, CssSelectorsDeleter>;
using SelectorMatcherPtr = std::unique_ptr<lxb_selectors_t, SelectorMatcherDeleter>;

// A CSS parser and its tokenizer, which it uses without owning it: lxb_css_parser_init leaks the tokenizer it creates
// when it fails after its creation (lexbor 3.0.1, still on master of 2026-11-16).
struct CssParser {
  CssTokenizerPtr pTokenizer;  // first: destroyed after the parser
  CssParserPtr pParser;
};

// A new CSS parser, with its tokenizer.
CssParser MakeCssParser();

// A new, small, CSS memory, for the selector lists of a parser.
CssMemoryPtr MakeCssMemory();

// A new parsing state of selectors, to give to a CSS parser (lxb_css_parser_selectors_set).
CssSelectorsPtr MakeCssSelectors();

// A new selector matching engine, each element being reported once even if it matches several selectors of a list.
SelectorMatcherPtr MakeSelectorMatcher();

// Parses 'selector' with pParser into its memory (lxb_css_parser_memory_set). Returns null if it is not a valid
// selector list. Throws std::bad_alloc on memory exhaustion.
[[nodiscard]] lxb_css_selector_list_t* ParseSelectorList(lxb_css_parser_t* pParser, std::string_view selector);

// Throws the std::invalid_argument of the public queries for 'selector', not a valid selector list.
[[noreturn]] void ThrowInvalidSelector(std::string_view selector);

}  // namespace aeronet::internal
