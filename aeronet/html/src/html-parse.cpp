#include "html-parse.hpp"

#include <lexbor/core/array.h>
#include <lexbor/core/types.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/html/interface.h>
#include <lexbor/html/interfaces/document.h>
#include <lexbor/html/parser.h>
#include <lexbor/html/token.h>
#include <lexbor/html/tokenizer.h>
#include <lexbor/html/tree.h>
#include <lexbor/ns/const.h>
#include <lexbor/tag/const.h>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <type_traits>

#include "lexbor-utils.hpp"

namespace aeronet::internal {

namespace {

// Placed between the tokenizer and the tree builder of a document during its parsing.
struct TreeLimiter {
  lxb_html_tree_t* pTree;
  lxb_html_tokenizer_token_f treeCallback;  // the tree builder, called for each token
  void* pTreeCallbackCtx;
};

lxb_html_token_t* LimitTree(lxb_html_tokenizer_t* pTokenizer, lxb_html_token_t* pToken, void* pCtx);

// Places a TreeLimiter between the tokenizer and the tree builder of pParser, for its lifetime: the parser stays with
// the document, and parses its fragments.
class TreeLimiterScope {
 public:
  explicit TreeLimiterScope(lxb_html_parser_t* pParser) noexcept
      : _pTokenizer(pParser->tkz),
        _limiter{pParser->tree, _pTokenizer->callback_token_done, _pTokenizer->callback_token_ctx} {
    lxb_html_tokenizer_callback_token_done_set(_pTokenizer, &LimitTree, &_limiter);
  }

  TreeLimiterScope(const TreeLimiterScope&) = delete;
  TreeLimiterScope& operator=(const TreeLimiterScope&) = delete;
  TreeLimiterScope(TreeLimiterScope&&) = delete;
  TreeLimiterScope& operator=(TreeLimiterScope&&) = delete;

  ~TreeLimiterScope() {
    lxb_html_tokenizer_callback_token_done_set(_pTokenizer, _limiter.treeCallback, _limiter.pTreeCallbackCtx);
  }

 private:
  lxb_html_tokenizer_t* _pTokenizer;
  TreeLimiter _limiter;
};

// Created by the parsing of the document, kept with it.
lxb_html_parser_t* Parser(lxb_html_document_t* pDocument) noexcept {
  auto* pParser = static_cast<lxb_html_parser_t*>(pDocument->dom_document.parser);
  assert(pParser != nullptr);
  return pParser;
}

bool IsStartTag(const lxb_html_token_t& token) noexcept {
  // The token types are int flags.
  return token.tag_id >= LXB_TAG_A &&
         (static_cast<unsigned>(token.type) & static_cast<unsigned>(LXB_HTML_TOKEN_TYPE_CLOSE)) == 0;
}

// Processes the end tag of the current node, as if it were in the input: the tree builder keeps a consistent state.
void ProcessEndTagOfCurrentNode(const TreeLimiter& limiter, lxb_html_tokenizer_t* pTokenizer) {
  lxb_html_token_t endTag{};
  endTag.tag_id = lxb_html_tree_current_node(limiter.pTree)->local_name;
  endTag.type = LXB_HTML_TOKEN_TYPE_CLOSE;
  [[maybe_unused]] const lxb_html_token_t* pProcessed =
      limiter.treeCallback(pTokenizer, &endTag, limiter.pTreeCallbackCtx);
  assert(pProcessed != nullptr);  // closing elements does not allocate
}

// Removes the earliest entries after the last marker of the list of active formatting elements beyond
// kMaxActiveFormattingElements. A formatting start tag adds one entry. The markers are a static element of lexbor,
// zero-initialized: of undefined tag.
void TrimActiveFormattingElements(lexbor_array_t& list) noexcept {
  if (list.length <= kMaxActiveFormattingElements) {
    return;
  }
  std::size_t first = list.length;
  while (first != 0 && static_cast<const lxb_dom_node_t*>(list.list[first - 1])->local_name != LXB_TAG__UNDEF) {
    --first;
  }
  const std::size_t count = list.length - first;
  if (count > kMaxActiveFormattingElements) {
    lexbor_array_delete(&list, first, count - kMaxActiveFormattingElements);
  }
}

lxb_html_token_t* LimitTree(lxb_html_tokenizer_t* pTokenizer, lxb_html_token_t* pToken, void* pCtx) {
  const TreeLimiter& limiter = *static_cast<const TreeLimiter*>(pCtx);
  lxb_html_tree_t& tree = *limiter.pTree;
  if (IsStartTag(*pToken)) {
    // The end tag of a formatting element can leave the current node open, removing instead from the list a later
    // entry of the same name that is no longer open (adoption agency algorithm): it is repeated as long as it does so.
    for (std::size_t formatting = std::numeric_limits<std::size_t>::max();
         tree.open_elements->length >= kMaxOpenElements && tree.active_formatting->length < formatting;) {
      formatting = tree.active_formatting->length;
      ProcessEndTagOfCurrentNode(limiter, pTokenizer);
    }
  }
  pToken = limiter.treeCallback(pTokenizer, pToken, limiter.pTreeCallbackCtx);
  TrimActiveFormattingElements(*tree.active_formatting);
  return pToken;
}

}  // namespace

void ParseHtml(lxb_html_document_t* pDocument, std::string_view html) {
  CheckStatus(lxb_html_document_parse_chunk_begin(pDocument));
  const TreeLimiterScope limiter(Parser(pDocument));
  CheckStatus(lxb_html_document_parse_chunk(pDocument, reinterpret_cast<const lxb_char_t*>(html.data()), html.size()));
  CheckStatus(lxb_html_document_parse_chunk_end(pDocument));
}

static_assert(std::is_same_v<lxb_tag_id_t, std::uintptr_t> && std::is_same_v<lxb_ns_id_t, std::uintptr_t>);

// Through the parser of the document, as lxb_html_document_parse_fragment, which takes the context from an element.
// A failed step destroys the fragment and leaves the parser to be cleaned by the next parsing.

lxb_dom_node_t* ParseHtmlFragment(lxb_html_document_t* pDocument, std::uintptr_t contextTag, std::uintptr_t contextNs,
                                  std::string_view html) {
  lxb_html_parser_t* pParser = Parser(pDocument);
  CheckStatus(lxb_html_parse_fragment_chunk_begin(pParser, pDocument, contextTag, contextNs));
  const TreeLimiterScope limiter(pParser);
  CheckStatus(
      lxb_html_parse_fragment_chunk_process(pParser, reinterpret_cast<const lxb_char_t*>(html.data()), html.size()));
  return CheckCreated(lxb_html_parse_fragment_chunk_end(pParser));
}

}  // namespace aeronet::internal
