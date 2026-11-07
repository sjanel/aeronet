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
#include <lexbor/tag/const.h>

#include <cassert>
#include <cstddef>
#include <limits>
#include <string_view>

#include "lexbor-utils.hpp"

namespace aeronet::internal {

namespace {

// Placed between the tokenizer and the tree builder of a document during its parsing.
struct TreeLimiter {
  lxb_html_tree_t* pTree;
  lxb_html_tokenizer_token_f treeCallback;  // the tree builder, called for each token
  void* pTreeCallbackCtx;
};

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
  auto* pParser = static_cast<lxb_html_parser_t*>(pDocument->dom_document.parser);
  lxb_html_tokenizer_t* pTokenizer = pParser->tkz;
  TreeLimiter limiter{pParser->tree, pTokenizer->callback_token_done, pTokenizer->callback_token_ctx};
  lxb_html_tokenizer_callback_token_done_set(pTokenizer, &LimitTree, &limiter);
  CheckStatus(lxb_html_document_parse_chunk(pDocument, reinterpret_cast<const lxb_char_t*>(html.data()), html.size()));
  CheckStatus(lxb_html_document_parse_chunk_end(pDocument));
  // The parser stays with the document, the limiter does not.
  lxb_html_tokenizer_callback_token_done_set(pTokenizer, limiter.treeCallback, limiter.pTreeCallbackCtx);
}

}  // namespace aeronet::internal
