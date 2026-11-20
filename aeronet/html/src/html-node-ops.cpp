#include "html-node-ops.hpp"

#include <lexbor/core/base.h>
#include <lexbor/core/str.h>
#include <lexbor/core/types.h>
#include <lexbor/dom/interfaces/attr.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/comment.h>
#include <lexbor/dom/interfaces/document.h>
#include <lexbor/dom/interfaces/document_fragment.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/dom/interfaces/text.h>
#include <lexbor/url/url.h>

#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <string_view>

#include "aeronet/html-element.hpp"
#include "aeronet/raw-chars.hpp"
#include "html-node-access.hpp"
#include "html-tree-walk.hpp"
#include "lexbor-utils.hpp"

namespace aeronet::internal {

namespace {

const lxb_char_t* Chars(std::string_view str) noexcept {
  // lexbor reads no byte of an empty string, but some of its functions refuse a null pointer.
  return reinterpret_cast<const lxb_char_t*>(str.empty() ? "" : str.data());
}

// Parses 'input' against pBase (can be null). Null when it is not a valid URL, which is also how lexbor reports most
// allocation failures of its URL parser.
lxb_url_t* ParseUrl(lxb_url_parser_t* pParser, const lxb_url_t* pBase, std::string_view input) {
  lxb_url_parser_clean(pParser);
  return lxb_url_parse(pParser, pBase, Chars(input), input.size());
}

// The memory of pNode alone, with its attributes.
std::size_t NodeMemory(const lxb_dom_node_t* pNode) noexcept {
  std::size_t memory = kNodeMemory;
  if (pNode->type == LXB_DOM_NODE_TYPE_ELEMENT) {
    for (const lxb_dom_attr_t* pAttr = reinterpret_cast<const lxb_dom_element_t*>(pNode)->first_attr; pAttr != nullptr;
         pAttr = pAttr->next) {
      memory += kNodeMemory + (pAttr->value != nullptr ? pAttr->value->length : 0);
    }
  } else {
    assert(IsTextNode(pNode) || pNode->type == LXB_DOM_NODE_TYPE_COMMENT);
    memory += reinterpret_cast<const lxb_dom_character_data_t*>(pNode)->data.length;
  }
  return memory;
}

// Whether pNode is the contents of a template (a document fragment out of the tree, whose host is the template).
bool IsTemplateContents(const lxb_dom_node_t* pNode) noexcept {
  return pNode != nullptr && pNode->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT &&
         reinterpret_cast<const lxb_dom_document_fragment_t*>(pNode)->host != nullptr;
}

// The node after pNode in the walk of the subtree of pTop, in pre-order, the contents of its templates included: those
// of a template come before its children. Null after the last one. Iterative: the templates can be nested at any
// depth by the scripts.
lxb_dom_node_t* NextInSubtree(lxb_dom_node_t* pNode, const lxb_dom_node_t* pTop) noexcept {
  if (const lxb_dom_node_t* pContents = ChildrenParent(pNode);
      pContents != pNode && pContents->first_child != nullptr) {
    return pContents->first_child;
  }
  if (pNode->first_child != nullptr) {
    return pNode->first_child;
  }
  while (pNode != pTop) {
    if (pNode->next != nullptr) {
      return pNode->next;
    }
    if (IsTemplateContents(pNode->parent)) {
      // The contents of a template are walked: then its children.
      pNode = &reinterpret_cast<lxb_dom_document_fragment_t*>(pNode->parent)->host->node;
      if (pNode->first_child != nullptr) {
        return pNode->first_child;
      }
    } else {
      pNode = pNode->parent;
    }
  }
  return nullptr;
}

}  // namespace

std::size_t NodesMemory(lxb_dom_node_t* pFirst, const lxb_dom_node_t* pEnd) noexcept {
  std::size_t memory = 0;
  for (lxb_dom_node_t* pTop = pFirst; pTop != pEnd; pTop = pTop->next) {
    lxb_dom_node_t* pNode = pTop;
    do {
      memory += NodeMemory(pNode);
      pNode = NextInSubtree(pNode, pTop);
    } while (pNode != nullptr);
  }
  return memory;
}

lxb_dom_node_t* NewTextNode(lxb_dom_node_t* pNode, std::string_view text) {
  return &CheckCreated(lxb_dom_document_create_text_node(pNode->owner_document, Chars(text), text.size()))
              ->char_data.node;
}

lxb_dom_node_t* NewCommentNode(lxb_dom_node_t* pNode, std::string_view text) {
  return &CheckCreated(lxb_dom_document_create_comment(pNode->owner_document, Chars(text), text.size()))
              ->char_data.node;
}

void SetNodeData(lxb_dom_node_t* pNode, std::string_view data) {
  assert(pNode->type == LXB_DOM_NODE_TYPE_TEXT || pNode->type == LXB_DOM_NODE_TYPE_COMMENT ||
         pNode->type == LXB_DOM_NODE_TYPE_CDATA_SECTION);
  // The character data interfaces start with their node. The whole data is replaced: lexbor ignores the range.
  CheckStatus(lxb_dom_character_data_replace(reinterpret_cast<lxb_dom_character_data_t*>(pNode), Chars(data),
                                             data.size(), 0, 0));
}

void InsertNode(lxb_dom_node_t* pParent, lxb_dom_node_t* pNode, lxb_dom_node_t* pReference) noexcept {
  assert(pNode->parent == nullptr);
  if (pReference != nullptr) {
    assert(pReference->parent == pParent);
    lxb_dom_node_insert_before(pReference, pNode);
  } else {
    lxb_dom_node_insert_child(pParent, pNode);
  }
}

void RemoveNode(lxb_dom_node_t* pNode) noexcept { lxb_dom_node_remove(pNode); }

InsertionPoint AdjacentInsertionPoint(lxb_dom_node_t* pElement, HtmlInsertPosition position) noexcept {
  switch (position) {
    case HtmlInsertPosition::BeforeBegin:
      return {pElement->parent, pElement};
    case HtmlInsertPosition::AfterBegin:
      return {pElement, pElement->first_child};
    case HtmlInsertPosition::BeforeEnd:
      return {pElement, nullptr};
    default:
      assert(position == HtmlInsertPosition::AfterEnd);
      return {pElement->parent, pElement->next};
  }
}

bool CanInsertAdjacent(const InsertionPoint& point) noexcept {
  return point.pParent != nullptr && point.pParent->type != LXB_DOM_NODE_TYPE_DOCUMENT;
}

std::string_view DomTagName(lxb_dom_node_t* pElement) {
  std::size_t size = 0;
  // The upper-case name of an HTML element is added to the names of its document by the first call.
  const lxb_char_t* pName = CheckCreated(lxb_dom_element_tag_name(AsElement(pElement), &size));
  return {reinterpret_cast<const char*>(pName), size};
}

UrlResolver::UrlResolver() {
  // The memory of the URLs is created by the initialization, and destroyed with them by the destructor.
  CheckStatus(lxb_url_parser_init(&_parser, nullptr));
}

UrlResolver::~UrlResolver() {
  lxb_url_parser_memory_destroy(&_parser);
  lxb_url_parser_destroy(&_parser, false);
}

std::string_view UrlResolver::resolve(std::string_view base, std::string_view input) {
  if (std::string_view(_base) != base) {
    _pBase = lxb_url_destroy(_pBase);
    _base.clear();
    if (!base.empty()) {
      _base.ensureAvailableCapacity(base.size());  // before the parsing: nothing fails once the base is parsed
      _pBase = ParseUrl(&_parser, nullptr, base);
      if (_pBase == nullptr) {
        return {};  // not kept: the failure can be an allocation failure
      }
      _base.unchecked_append(base);
    }
  }
  lxb_url_t* pUrl = ParseUrl(&_parser, _pBase, input);
  _url.clear();
  if (pUrl == nullptr) {
    return {};
  }
  SerializationAppender<RawChars> appender(_url);
  const lxb_status_t status = lxb_url_serialize(pUrl, &SerializationAppender<RawChars>::Append, &appender, false);
  lxb_url_destroy(pUrl);
  appender.finish(status);
  _url.push_back('\0');
  _url.setSize(_url.size() - 1U);
  return _url;
}

std::string_view UrlResolver::resolve(std::string_view base, std::initializer_list<std::string_view> inputParts) {
  std::size_t size = 0;
  for (const std::string_view part : inputParts) {
    size += part.size();
  }
  _input.clear();
  _input.ensureAvailableCapacity(size);
  for (const std::string_view part : inputParts) {
    _input.unchecked_append(part);
  }
  return resolve(base, std::string_view(_input));
}

}  // namespace aeronet::internal
