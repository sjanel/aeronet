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
#include <lexbor/html/interfaces/template_element.h>
#include <lexbor/ns/ns.h>
#include <lexbor/url/url.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <string_view>

#include "aeronet/html-element.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"
#include "html-node-access.hpp"
#include "html-tree-walk.hpp"
#include "lexbor-utils.hpp"

namespace aeronet::internal {

namespace {

// Parses 'input' against pBase (can be null). Null when it is not a valid URL, which is also how lexbor reports most
// allocation failures of its URL parser.
lxb_url_t* ParseUrl(lxb_url_parser_t* pParser, const lxb_url_t* pBase, std::string_view input) {
  lxb_url_parser_clean(pParser);
  return lxb_url_parse(pParser, pBase, Chars(input), input.size());
}

// Whether pNode is the contents of a template (a document fragment out of the tree, whose host is the template).
bool IsTemplateContents(const lxb_dom_node_t* pNode) noexcept {
  return pNode != nullptr && pNode->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT &&
         reinterpret_cast<const lxb_dom_document_fragment_t*>(pNode)->host != nullptr;
}

// Whether pNode is the ancestor of another node: it has children, or it is a template whose contents have some.
bool HasDescendants(const lxb_dom_node_t* pNode) noexcept {
  return pNode->first_child != nullptr ||
         (IsHtmlTemplate(pNode) &&
          reinterpret_cast<const lxb_html_template_element_t*>(pNode)->content->node.first_child != nullptr);
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

lxb_dom_node_t* NewDocumentFragment(lxb_dom_node_t* pNode) {
  return &CheckCreated(lxb_dom_document_create_document_fragment(pNode->owner_document))->node;
}

lxb_dom_node_t* NewElementNs(lxb_dom_node_t* pNode, std::string_view namespaceUri, std::string_view qualifiedName) {
  const std::size_t colon = qualifiedName.find(':');
  const std::string_view localName = colon == std::string_view::npos ? qualifiedName : qualifiedName.substr(colon + 1);
  const std::string_view prefix = colon == std::string_view::npos ? std::string_view{} : qualifiedName.substr(0, colon);

  // A null namespace or prefix is a null pointer for lexbor.
  return &CheckCreated(lxb_dom_element_create(pNode->owner_document, Chars(localName), localName.size(),
                                              namespaceUri.empty() ? nullptr : Chars(namespaceUri), namespaceUri.size(),
                                              prefix.empty() ? nullptr : Chars(prefix), prefix.size(), nullptr, 0,
                                              false))
              ->node;
}

std::string_view NamespaceUri(const lxb_dom_node_t* pElement) noexcept {
  std::size_t size = 0;
  // Null with a size of 0 for an unknown namespace.
  const lxb_char_t* pUri = lxb_ns_by_id(pElement->owner_document->ns, pElement->ns, &size);
  return {reinterpret_cast<const char*>(pUri), size};
}

namespace {

// A copy of pNode alone (with its attributes for an element), without user pointer, reported to onCloned (can be null).
lxb_dom_node_t* CloneOne(lxb_dom_node_t* pNode, ClonedFn onCloned, void* pData) {
  lxb_dom_node_t* pCopy = nullptr;
  if (pNode->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT) {
    // lexbor 3.0.1 copies a fragment into a plain node, smaller than a fragment (lxb_html_interface_clone): reading its
    // host would overflow it.
    pCopy = NewDocumentFragment(pNode);
  } else {
    lxb_dom_document_t* pDocument = pNode->owner_document;
    pCopy = static_cast<lxb_dom_node_t*>(CheckCreated(pDocument->clone_interface(pDocument, pNode)));
    pCopy->user = nullptr;
    if (pNode->type == LXB_DOM_NODE_TYPE_ELEMENT) {
      // lexbor 3.0.1 copies the local name and the prefix, not the qualified name: the names of its document, shared.
      lxb_dom_element_t* pElementCopy = AsElement(pCopy);
      pElementCopy->qualified_name = AsElement(pNode)->qualified_name;
      pElementCopy->upper_name = AsElement(pNode)->upper_name;
    }
  }
  if (onCloned != nullptr) {
    onCloned(pNode, pCopy, pData);
  }
  return pCopy;
}

// A node whose children are copied into its copy.
struct CopiedParent {
  lxb_dom_node_t* pSource;
  lxb_dom_node_t* pCopy;
};

}  // namespace

lxb_dom_node_t* CloneNode(lxb_dom_node_t* pNode, bool deep, ClonedFn onCloned, void* pData) {
  lxb_dom_node_t* pCopy = CloneOne(pNode, onCloned, pData);
  if (!deep) {
    return pCopy;
  }
  // The parents whose children are left to copy: each copy is a child of its parent copy, in order.
  vector<CopiedParent> parents(1U, CopiedParent(pNode, pCopy));
  while (!parents.empty()) {
    const CopiedParent parent = parents.pop_back_val();
    if (lxb_dom_node_t* pContents = ChildrenParent(parent.pSource); pContents != parent.pSource) {
      // A template, whose copy has its own empty contents (HTML "cloning steps" of a template).
      parents.emplace_back(pContents, ChildrenParent(parent.pCopy));
    }
    for (lxb_dom_node_t* pChild = parent.pSource->first_child; pChild != nullptr; pChild = pChild->next) {
      lxb_dom_node_t* pChildCopy = CloneOne(pChild, onCloned, pData);
      InsertNode(parent.pCopy, pChildCopy, nullptr);
      if (pChild->first_child != nullptr || ChildrenParent(pChild) != pChild) {
        parents.emplace_back(pChild, pChildCopy);
      }
    }
  }
  return pCopy;
}

void SetNodeData(lxb_dom_node_t* pNode, std::string_view data) {
  assert(pNode->type == LXB_DOM_NODE_TYPE_TEXT || pNode->type == LXB_DOM_NODE_TYPE_COMMENT ||
         pNode->type == LXB_DOM_NODE_TYPE_CDATA_SECTION);
  // The character data interfaces start with their node. The whole data is replaced: lexbor ignores the range.
  CheckStatus(lxb_dom_character_data_replace(reinterpret_cast<lxb_dom_character_data_t*>(pNode), Chars(data),
                                             data.size(), 0, 0));
}

bool IsHostIncludingInclusiveAncestor(const lxb_dom_node_t* pNode, const lxb_dom_node_t* pParent) noexcept {
  // A node without descendants (an element just created, a text node) is only its own ancestor: no walk up the tree,
  // which would make the insertions of new nodes at a growing depth quadratic.
  if (!HasDescendants(pNode)) {
    return pNode == pParent;
  }
  const lxb_dom_node_t* pAncestor = pParent;
  while (pAncestor != nullptr && pAncestor != pNode) {
    // The contents of a template have no parent: the walk goes on with the template.
    pAncestor = IsTemplateContents(pAncestor)
                    ? &reinterpret_cast<const lxb_dom_document_fragment_t*>(pAncestor)->host->node
                    : pAncestor->parent;
  }
  return pAncestor != nullptr;
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
  _input.clear();
  _input.reserve(std::ranges::fold_left(inputParts, std::size_t{0},
                                        [](std::size_t sum, std::string_view part) { return sum + part.size(); }));
  char* pData = _input.data();
  for (const std::string_view part : inputParts) {
    pData = Append(part, pData);
  }
  _input.setEnd(pData);
  return resolve(base, std::string_view(_input));
}

}  // namespace aeronet::internal
