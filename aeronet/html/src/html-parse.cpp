#include "html-parse.hpp"

#include <lexbor/core/array.h>
#include <lexbor/core/array_obj.h>
#include <lexbor/core/base.h>
#include <lexbor/core/dobject.h>
#include <lexbor/core/hash.h>
#include <lexbor/core/lexbor.h>
#include <lexbor/core/mraw.h>
#include <lexbor/core/str.h>
#include <lexbor/core/types.h>
#include <lexbor/dom/interfaces/attr.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/document.h>
#include <lexbor/dom/interfaces/document_fragment.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/html/interface.h>
#include <lexbor/html/interfaces/document.h>
#include <lexbor/html/interfaces/template_element.h>
#include <lexbor/html/parser.h>
#include <lexbor/html/token.h>
#include <lexbor/html/token_attr.h>
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
#include <utility>

#include "lexbor-utils.hpp"

namespace aeronet::internal {

namespace {

// A text node seen before an insertion place after a token, with the address of its data then. The node is compared,
// never dereferenced: it may have been destroyed since.
struct SeenText {
  const lxb_dom_node_t* pNode;
  const lxb_char_t* pData;
};

// Placed between the tokenizer and the tree builder of a document during its parsing.
struct TreeLimiter {
  lxb_html_tree_t* pTree;
  lxb_html_tokenizer_token_f treeCallback;  // the tree builder, called for each token
  void* pTreeCallbackCtx;
  SeenText inCurrentNode;   // the last child of the current node
  SeenText fosterParented;  // before the table of the current node, where its characters that are not whitespace go
};

lxb_html_token_t* LimitTree(lxb_html_tokenizer_t* pTokenizer, lxb_html_token_t* pToken, void* pCtx);

// Places a TreeLimiter between the tokenizer and the tree builder of pParser, for its lifetime: the parser stays with
// the document, and parses its fragments.
class TreeLimiterScope {
 public:
  explicit TreeLimiterScope(lxb_html_parser_t* pParser) noexcept
      : _pTokenizer(pParser->tkz),
        _limiter{pParser->tree, _pTokenizer->callback_token_done, _pTokenizer->callback_token_ctx, {}, {}} {
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

// Destroys the lists where the tokenizer and the tree builder of 'parser' record the parse errors, which aeronet never
// reads: without them, lexbor records nothing. Kept with the document, each error took 128 or 192 bytes
// (lexbor_array_obj allocates 8 times the size of its entries), 22 MB for 576 KB of ignored tags, and the lists grew by
// 128 entries, each growth copying them where realloc cannot extend them in place (quadratic time).
void DropParseErrors(lxb_html_parser_t& parser) noexcept {
  // The destroy functions of lexbor return NULL, for their callers to reset the pointers they freed.
  parser.tkz->parse_errors = lexbor_array_obj_destroy(parser.tkz->parse_errors, true);
  parser.tree->parse_errors = lexbor_array_obj_destroy(parser.tree->parse_errors, true);
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

// The number of attributes of 'element', an <html> or <body> element that the parsing adds attributes to. It created
// the element (a fragment has its own <html> root), and the scripts of a page run once the parsing ended: at most
// kMaxAttributes.
std::size_t AttributeCount(const lxb_dom_element_t& element) noexcept {
  std::size_t count = 0;
  for (const lxb_dom_attr_t* pAttr = element.first_attr; pAttr != nullptr; pAttr = pAttr->next) {
    ++count;
  }
  assert(count <= kMaxAttributes);
  return count;
}

// Whether the tree builder processes a start tag other than <svg>, <mglyph> and <malignmark> by the rules of foreign
// content (as lxb_html_tree_construction_dispatcher()): in an SVG or MathML element that is not an integration point,
// where such a tag, <html> included, creates an element of that namespace. Some elements are open.
bool InForeignContent(lxb_html_tree_t& tree) noexcept {
  lxb_dom_node_t* pNode = lxb_html_tree_adjusted_current_node(&tree);
  assert(pNode != nullptr);
  return pNode->ns != LXB_NS_HTML && !lxb_html_tree_mathml_text_integration_point(pNode) &&
         !lxb_html_tree_html_integration_point(pNode);
}

// The existing element that 'token', an <html> or <body> start tag, adds its attributes to, if any, as the tree builder
// finds it (which may also ignore the tag): the first open element for <html> out of foreign content, the second one,
// if a <body>, for <body> (which leaves foreign content first).
const lxb_dom_element_t* ElementTakingAttributesOf(lxb_html_tree_t& tree, const lxb_html_token_t& token) noexcept {
  const lexbor_array_t& openElements = *tree.open_elements;
  if (token.tag_id == LXB_TAG_HTML && openElements.length != 0 && !InForeignContent(tree)) {
    return static_cast<const lxb_dom_element_t*>(openElements.list[0]);
  }
  if (token.tag_id == LXB_TAG_BODY && openElements.length > 1 &&
      static_cast<const lxb_dom_node_t*>(openElements.list[1])->local_name == LXB_TAG_BODY) {
    return static_cast<const lxb_dom_element_t*>(openElements.list[1]);
  }
  return nullptr;
}

// Keeps the first attributes of 'token', a start tag, that its element can take (kMaxAttributes in all), and returns
// the others, with their values, to the memory of the tokenizer: lexbor frees the attributes of tokens only once the
// parsing ends.
void LimitAttributes(lxb_html_tokenizer_t& tokenizer, lxb_html_tree_t& tree, lxb_html_token_t& token) noexcept {
  lxb_html_token_attr_t* pAttr = token.attr_first;
  if (pAttr == nullptr) {
    return;
  }
  const lxb_dom_element_t* pElement = ElementTakingAttributesOf(tree, token);
  for (std::size_t room = kMaxAttributes - (pElement == nullptr ? 0 : AttributeCount(*pElement));
       pAttr != nullptr && room != 0; --room) {
    pAttr = pAttr->next;
  }
  if (pAttr == nullptr) {
    return;
  }
  token.attr_last = pAttr->prev;
  if (pAttr->prev == nullptr) {
    token.attr_first = nullptr;
  } else {
    pAttr->prev->next = nullptr;
  }
  // The free and destroy functions of lexbor return NULL, for their callers to reset the pointers they freed.
  do {
    lxb_html_token_attr_t* pNext = pAttr->next;
    if (pAttr->value != nullptr) {
      static_cast<void>(lexbor_mraw_free(tokenizer.attrs_mraw, pAttr->value));
    }
    static_cast<void>(lxb_html_token_attr_destroy(pAttr, tokenizer.dobj_token_attr));
    pAttr = pNext;
  } while (pAttr != nullptr);
}

// Gives the data of pNode, if a text node, twice its length in capacity when it moved since 'seen' was recorded, then
// records it. Returns false on memory exhaustion.
// The tree builder appends the characters of a token to the text node before their insertion place, growing its data
// to the exact size needed in the text memory of the document: in place when that data ends the memory, else in a new
// block, the old one being reusable only by smaller allocations. Markup that allocates text memory without inserting a
// node at that place (attribute values of ignored tags, comments in a table...) made each append copy the whole text
// to a new block, in quadratic memory: 400 KB of '<tr><td class="c">12.5</td><td>BTC</td></tr>' outside of a table
// took 154 MB. With capacity doubled after each move, the moves, and the memory they leave, become linear.
[[nodiscard]] bool ReserveMovedText(lexbor_mraw_t* pTextMemory, lxb_dom_node_t* pNode, SeenText& seen) noexcept {
  if (pNode == nullptr || pNode->local_name != LXB_TAG__TEXT) {
    return true;
  }
  lexbor_str_t& data = reinterpret_cast<lxb_dom_character_data_t*>(pNode)->data;
  if (pNode == seen.pNode && data.data != seen.pData) {
    const std::size_t capacity = 2 * (data.length + 1);  // lexbor ends the data with a null character
    if (capacity > lexbor_mraw_data_size(data.data)) {
      auto* pData = static_cast<lxb_char_t*>(lexbor_mraw_realloc(pTextMemory, data.data, capacity));
      if (pData == nullptr) {
        return false;
      }
      data.data = pData;
    }
  }
  seen = {pNode, data.data};
  return true;
}

// Whether the characters of a table that are not whitespace are inserted before it (foster parenting) when 'node' is
// the current node.
bool FosterParentsCharacters(const lxb_dom_node_t& node) noexcept {
  return node.ns == LXB_NS_HTML &&
         (node.local_name == LXB_TAG_TABLE || node.local_name == LXB_TAG_TBODY || node.local_name == LXB_TAG_TFOOT ||
          node.local_name == LXB_TAG_THEAD || node.local_name == LXB_TAG_TR);
}

// The last child of the place where the tree builder inserts characters without foster parenting, the current node or
// the contents of a template (as lxb_html_tree_appropriate_place_inserting_node(), not called on this hot path): the
// text node they are appended to, if it is one.
lxb_dom_node_t* LastChildOfInsertionPlace(lxb_dom_node_t& currentNode) noexcept {
  if (currentNode.local_name == LXB_TAG_TEMPLATE && currentNode.ns == LXB_NS_HTML) {
    return reinterpret_cast<lxb_html_template_element_t&>(currentNode).content->node.last_child;
  }
  return currentNode.last_child;
}

// The node before the place where the tree builder inserts the characters of a table that are not whitespace (foster
// parenting): the text node they are appended to, if it is one.
lxb_dom_node_t* NodeBeforeFosterParentedCharacters(lxb_html_tree_t& tree) noexcept {
  const bool fosterParentingBefore = std::exchange(tree.foster_parenting, true);
  lxb_html_tree_insertion_position_t position{};
  lxb_dom_node_t* pPlace = lxb_html_tree_appropriate_place_inserting_node(&tree, nullptr, &position);
  tree.foster_parenting = fosterParentingBefore;
  assert(pPlace != nullptr);  // the current node is part of a table
  return position == LXB_HTML_TREE_INSERTION_POSITION_BEFORE ? pPlace->prev : pPlace->last_child;
}

// Whether the tree builder can append characters to a text node while processing 'token': a character token, or any
// token after the characters of a table, kept until then (they may be foster parented).
bool AppendsCharacters(const lxb_html_tree_t& tree, const lxb_html_token_t& token) noexcept {
  return token.tag_id == LXB_TAG__TEXT || tree.pending_table.text_list->length != 0;
}

// Calls ReserveMovedText() for the text nodes that characters were appended to, if any. Returns false on memory
// exhaustion.
[[nodiscard]] bool ReserveMovedTexts(TreeLimiter& limiter) noexcept {
  lxb_html_tree_t& tree = *limiter.pTree;
  lxb_dom_node_t* pCurrentNode = lxb_html_tree_current_node(&tree);
  if (pCurrentNode == nullptr) {
    return true;  // characters before <html> are not inserted
  }
  lexbor_mraw_t* pTextMemory = tree.document->dom_document.text;
  if (!ReserveMovedText(pTextMemory, LastChildOfInsertionPlace(*pCurrentNode), limiter.inCurrentNode)) {
    return false;
  }
  return !FosterParentsCharacters(*pCurrentNode) ||
         ReserveMovedText(pTextMemory, NodeBeforeFosterParentedCharacters(tree), limiter.fosterParented);
}

lxb_html_token_t* LimitTree(lxb_html_tokenizer_t* pTokenizer, lxb_html_token_t* pToken, void* pCtx) {
  TreeLimiter& limiter = *static_cast<TreeLimiter*>(pCtx);
  lxb_html_tree_t& tree = *limiter.pTree;
  if (IsStartTag(*pToken)) {
    // The end tag of a formatting element can leave the current node open, removing instead from the list a later
    // entry of the same name that is no longer open (adoption agency algorithm): it is repeated as long as it does so.
    for (std::size_t formatting = std::numeric_limits<std::size_t>::max();
         tree.open_elements->length >= kMaxOpenElements && tree.active_formatting->length < formatting;) {
      formatting = tree.active_formatting->length;
      ProcessEndTagOfCurrentNode(limiter, pTokenizer);
    }
    LimitAttributes(*pTokenizer, tree, *pToken);
  }
  const bool appendsCharacters = AppendsCharacters(tree, *pToken);
  pToken = limiter.treeCallback(pTokenizer, pToken, limiter.pTreeCallbackCtx);
  TrimActiveFormattingElements(*tree.active_formatting);
  if (appendsCharacters && pToken != nullptr && !ReserveMovedTexts(limiter)) {
    pTokenizer->status = LXB_STATUS_ERROR_MEMORY_ALLOCATION;
    return nullptr;
  }
  return pToken;
}

// The bucket of 'entry' in 'table', where lexbor looks it up: the hash id of its name modulo the number of buckets. The
// hash id of a name is the one of the key its entry stores: the variants interning names in lower or upper case store
// them converted, and hash them converted.
std::size_t BucketOf(const lexbor_hash_t& table, const lexbor_hash_entry_t& entry) noexcept {
  return lexbor_hash_make_id(lexbor_hash_entry_str(&entry), entry.length) % table.table_size;
}

#ifndef NDEBUG
// Whether lexbor finds 'entry' of 'table' by its name (the key of no other entry): checks that BucketOf() matches the
// lookups of the tested lexbor versions.
bool FoundByLexbor(lexbor_hash_t& table, const lexbor_hash_entry_t& entry) noexcept {
  return lexbor_hash_search(&table, lexbor_hash_search_raw, lexbor_hash_entry_str(&entry), entry.length) == &entry;
}
#endif

// The number of names in the bucket of 'entry', counted up to kMaxCollidingNames + 1.
std::size_t BucketSize(const lexbor_hash_t& table, const lexbor_hash_entry_t& entry) noexcept {
  std::size_t count = 0;
  for (const lexbor_hash_entry_t* pEntry = table.table[BucketOf(table, entry)];
       pEntry != nullptr && count <= kMaxCollidingNames; pEntry = pEntry->next) {
    ++count;
  }
  return count;
}

// A table of a document interning names (of tags or attributes) during a parsing, checked after each chunk of its
// input (see kMaxNamesPerBucket and kMaxCollidingNames).
class NameTableWatch {
 public:
  // First gives the table a bucket per kHtmlBytesPerNameBucket bytes of an input of 'htmlSize' bytes, if it has fewer.
  NameTableWatch(lexbor_hash_t& table, std::size_t htmlSize) noexcept
      : _pTable(&table),
        _pChunk(table.entries->mem->chunk),
        _offset(_pChunk->length),
        _maxNames(Reserve(table, htmlSize / kHtmlBytesPerNameBucket)) {}

  // Grows the table to a bucket per name if it holds more than kMaxNamesPerBucket names per bucket, then returns false
  // if a bucket of a name interned since the previous call holds more than kMaxCollidingNames names. After a failed
  // growth, the table is no longer grown nor checked during this parsing: only its lookups are slower.
  [[nodiscard]] bool check() noexcept {
    const std::size_t nbNames = lexbor_dobject_allocated(_pTable->entries);
    if (nbNames > _maxNames) {
      _maxNames = GrowNameTable(*_pTable, nbNames) ? kMaxNamesPerBucket * nbNames : kNoLongerGrown;
    }
    return _maxNames == kNoLongerGrown || checkNewNames();
  }

 private:
  static constexpr std::size_t kNoLongerGrown = std::numeric_limits<std::size_t>::max();

  // Gives 'table' 'bucketCount' buckets if it has fewer (on memory exhaustion, it grows with its names later), and
  // returns the number of names beyond which it grows.
  static std::size_t Reserve(lexbor_hash_t& table, std::size_t bucketCount) noexcept {
    if (bucketCount > table.table_size) {
      static_cast<void>(GrowNameTable(table, bucketCount));
    }
    return kMaxNamesPerBucket * table.table_size;
  }

  // The names interned since the previous call follow the checked ones: lexbor allocates the entries of the names one
  // after the other in chunks of memory, and frees none during a successful parsing (only the entry of a name whose
  // allocation failed, which fails the parsing).
  bool checkNewNames() noexcept {
    const std::size_t entrySize = lexbor_mem_align(_pTable->entries->struct_size);
    while (true) {
      for (; _offset < _pChunk->length; _offset += entrySize) {
        const lexbor_hash_entry_t& entry = *reinterpret_cast<const lexbor_hash_entry_t*>(_pChunk->data + _offset);
        assert(FoundByLexbor(*_pTable, entry));
        if (BucketSize(*_pTable, entry) > kMaxCollidingNames) {
          return false;
        }
      }
      if (_pChunk->next == nullptr) {
        return true;
      }
      _pChunk = _pChunk->next;
      _offset = 0;
    }
  }

  lexbor_hash_t* _pTable;
  const lexbor_mem_chunk_t* _pChunk;  // with _offset, the entry of the first name not checked yet
  std::size_t _offset;
  std::size_t _maxNames;  // the number of names beyond which the table grows
};

// lxb_html_parse_chunk_process() or lxb_html_parse_fragment_chunk_process().
using ParseChunkFn = lxb_status_t (*)(lxb_html_parser_t*, const lxb_char_t*, std::size_t);

// Parses 'html' through pParser, parsing 'document' or one of its fragments, with 'parseChunk', in chunks of
// kParseChunkSize bytes. Stops before the end of 'html' when names collide in the tables of 'document'.
void ParseChunks(lxb_html_parser_t* pParser, ParseChunkFn parseChunk, lxb_dom_document_t& document,
                 std::string_view html) {
  NameTableWatch tagNames(*document.tags, html.size());
  NameTableWatch attributeNames(*document.attrs, html.size());
  while (!html.empty()) {
    const std::string_view chunk = html.substr(0, kParseChunkSize);
    CheckStatus(parseChunk(pParser, Chars(chunk), chunk.size()));
    html.remove_prefix(chunk.size());
    if (!tagNames.check() || !attributeNames.check()) {
      return;
    }
  }
}

}  // namespace

bool GrowNameTable(lexbor_hash_t& table, std::size_t bucketCount) noexcept {
  assert(bucketCount > table.table_size);
  auto** pBuckets = static_cast<lexbor_hash_entry_t**>(lexbor_calloc(bucketCount, sizeof(lexbor_hash_entry_t*)));
  if (pBuckets == nullptr) {
    return false;
  }
  lexbor_hash_entry_t** pOldBuckets = std::exchange(table.table, pBuckets);
  const std::size_t oldBucketCount = std::exchange(table.table_size, bucketCount);
  for (std::size_t idx = 0; idx < oldBucketCount; ++idx) {
    for (lexbor_hash_entry_t* pEntry = pOldBuckets[idx]; pEntry != nullptr;) {
      lexbor_hash_entry_t* pNext = pEntry->next;
      lexbor_hash_entry_t*& pBucket = pBuckets[BucketOf(table, *pEntry)];
      pEntry->next = pBucket;
      pBucket = pEntry;
      assert(FoundByLexbor(table, *pEntry));
      pEntry = pNext;
    }
  }
  static_cast<void>(lexbor_free(static_cast<void*>(pOldBuckets)));  // returns NULL, for the caller to reset its pointer
  return true;
}

void ParseHtml(lxb_html_document_t* pDocument, std::string_view html) {
  CheckStatus(lxb_html_document_parse_chunk_begin(pDocument));
  lxb_html_parser_t* pParser = Parser(pDocument);
  DropParseErrors(*pParser);  // its fragments are parsed by the same parser
  const TreeLimiterScope limiter(pParser);
  ParseChunks(pParser, &lxb_html_parse_chunk_process, pDocument->dom_document, html);
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
  ParseChunks(pParser, &lxb_html_parse_fragment_chunk_process, pDocument->dom_document, html);
  return CheckCreated(lxb_html_parse_fragment_chunk_end(pParser));
}

}  // namespace aeronet::internal
