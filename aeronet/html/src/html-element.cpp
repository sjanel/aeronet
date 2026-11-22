#include "aeronet/html-element.hpp"

#include <lexbor/core/base.h>
#include <lexbor/core/str.h>
#include <lexbor/core/types.h>
#include <lexbor/dom/interfaces/attr.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/element.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/html/serialize.h>

#include <cstddef>
#include <string>
#include <string_view>

#include "aeronet/memory-utils.hpp"
#include "html-node-access.hpp"
#include "html-tree-walk.hpp"
#include "lexbor-utils.hpp"

namespace aeronet {

namespace {

using internal::AsElement;
using internal::Checked;
using internal::HtmlNodeAccess;

std::string_view View(const lxb_char_t* pData, std::size_t size) noexcept {
  return {reinterpret_cast<const char*>(pData), size};
}

const lexbor_str_t& TextData(lxb_dom_node_t* pNode) noexcept {
  return reinterpret_cast<lxb_dom_character_data_t*>(pNode)->data;
}

// The first element among pNode and its siblings in the direction of 'next', or an invalid element.
template <lxb_dom_node_t* lxb_dom_node_t::* next>
HtmlElement FirstElementFrom(lxb_dom_node_t* pNode) noexcept {
  while (pNode != nullptr && pNode->type != LXB_DOM_NODE_TYPE_ELEMENT) {
    pNode = pNode->*next;
  }
  return HtmlNodeAccess::Element(pNode);
}

// The descendant text nodes of an element: the total size of their text, and the first one that is not empty.
struct TextNodes {
  std::string_view first;
  std::size_t size{};
};

TextNodes ScanTextNodes(lxb_dom_node_t* pRoot) noexcept {
  TextNodes text;
  internal::WalkDescendants(pRoot, [&text](lxb_dom_node_t* pNode) {
    if (internal::IsTextNode(pNode)) {
      const lexbor_str_t& data = TextData(pNode);
      if (text.size == 0) {
        text.first = View(data.data, data.length);
      }
      text.size += data.length;
    }
    return true;
  });
  return text;
}

// Replaces the contents of 'out' by the text of the descendant text nodes of pRoot, of total size 'size'.
void CopyTextNodes(lxb_dom_node_t* pRoot, std::size_t size, std::string& out) {
  out.clear();  // nothing to keep if the buffer grows
  out.resize_and_overwrite(size, [pRoot](char* pOut, std::size_t textSize) {
    internal::WalkDescendants(pRoot, [&pOut](lxb_dom_node_t* pNode) {
      if (internal::IsTextNode(pNode)) {
        const lexbor_str_t& data = TextData(pNode);
        pOut = Append(reinterpret_cast<const char*>(data.data), data.length, pOut);
      }
      return true;
    });
    return textSize;
  });
}

template <lxb_status_t (*Serialize)(lxb_dom_node_t*, lxb_html_serialize_cb_f, void*)>
void AppendSerializedNode(lxb_dom_node_t* pNode, std::string& out) {
  internal::SerializationAppender<std::string> appender(out);
  appender.finish(Serialize(pNode, &internal::SerializationAppender<std::string>::Append, &appender));
}

}  // namespace

// lexbor keeps the lower-cased name as local name, the name adjusted by the parser for SVG elements, or with its prefix
// for the elements of createElementNS() (aeronet_web), as qualified name.
std::string_view HtmlElement::tagName() const noexcept {
  std::size_t size = 0;
  const lxb_char_t* pName = lxb_dom_element_qualified_name(AsElement(_pNode), &size);
  return View(pName, size);
}

std::string_view HtmlElement::attribute(std::string_view name) const noexcept {
  std::size_t size = 0;
  const lxb_char_t* pValue = lxb_dom_element_get_attribute(
      AsElement(_pNode), reinterpret_cast<const lxb_char_t*>(name.data()), name.size(), &size);
  return View(pValue, size);
}

bool HtmlElement::hasAttribute(std::string_view name) const noexcept {
  return lxb_dom_element_has_attribute(AsElement(_pNode), reinterpret_cast<const lxb_char_t*>(name.data()),
                                       name.size());
}

HtmlAttributes HtmlElement::attributes() const noexcept { return HtmlAttributes(AsElement(_pNode)->first_attr); }

HtmlAttribute HtmlAttributes::iterator::operator*() const noexcept {
  std::size_t nameSize = 0;
  const lxb_char_t* pName = lxb_dom_attr_qualified_name(_pAttr, &nameSize);
  std::size_t valueSize = 0;
  const lxb_char_t* pValue = lxb_dom_attr_value(_pAttr, &valueSize);
  return {View(pName, nameSize), View(pValue, valueSize)};
}

HtmlAttributes::iterator::iterator(lxb_dom_attr_t* pAttr) noexcept
    : _pAttr(pAttr), _pNext(pAttr == nullptr ? nullptr : pAttr->next) {}

// Through _pNext: removing an attribute (lxb_dom_attr_remove) resets its next pointer.
HtmlAttributes::iterator& HtmlAttributes::iterator::operator++() noexcept {
  _pAttr = _pNext;
  _pNext = _pAttr == nullptr ? nullptr : _pAttr->next;
  return *this;
}

// A first walk sizes the text: it is copied by a second one, in a single allocation, unless it is in a single node.
std::string_view HtmlElement::textContent(std::string& buffer) const {
  const TextNodes text = ScanTextNodes(Checked(_pNode));
  if (text.size == text.first.size()) {
    return text.first;
  }
  CopyTextNodes(_pNode, text.size, buffer);
  return buffer;
}

std::string HtmlElement::textContent() const {
  std::string text;
  const std::string_view view = textContent(text);
  if (view.data() != text.data()) {
    text = view;  // the text is in the document
  }
  return text;
}

std::string HtmlElement::innerHtml() const {
  std::string html;
  appendInnerHtml(html);
  return html;
}

std::string HtmlElement::outerHtml() const {
  std::string html;
  appendOuterHtml(html);
  return html;
}

void HtmlElement::appendInnerHtml(std::string& out) const {
  AppendSerializedNode<lxb_html_serialize_deep_cb>(internal::ChildrenParent(Checked(_pNode)), out);
}

void HtmlElement::appendOuterHtml(std::string& out) const {
  AppendSerializedNode<lxb_html_serialize_tree_cb>(Checked(_pNode), out);
}

// The parent of an element is an element, the document node for the root element, or none out of the tree (the
// document fragment of a template contents cannot be reached).
HtmlElement HtmlElement::parent() const noexcept {
  lxb_dom_node_t* pParent = Checked(_pNode)->parent;
  return HtmlNodeAccess::Element(pParent != nullptr && pParent->type == LXB_DOM_NODE_TYPE_ELEMENT ? pParent : nullptr);
}

HtmlElement HtmlElement::firstChild() const noexcept {
  return FirstElementFrom<&lxb_dom_node_t::next>(Checked(_pNode)->first_child);
}

HtmlElement HtmlElement::lastChild() const noexcept {
  return FirstElementFrom<&lxb_dom_node_t::prev>(Checked(_pNode)->last_child);
}

HtmlElement HtmlElement::nextSibling() const noexcept {
  return FirstElementFrom<&lxb_dom_node_t::next>(Checked(_pNode)->next);
}

HtmlElement HtmlElement::previousSibling() const noexcept {
  return FirstElementFrom<&lxb_dom_node_t::prev>(Checked(_pNode)->prev);
}

}  // namespace aeronet
