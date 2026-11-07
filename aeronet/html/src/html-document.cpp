#include "aeronet/html-document.hpp"

#include <lexbor/core/types.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/document.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/encoding/base.h>
#include <lexbor/html/interfaces/document.h>
#include <lexbor/tag/const.h>

#include <cassert>
#include <memory>
#include <string_view>
#include <utility>

#include "aeronet/html-element.hpp"
#include "aeronet/raw-chars.hpp"
#include "assigned-json.hpp"
#include "html-encoding.hpp"
#include "html-node-access.hpp"
#include "html-parse.hpp"
#include "html-query-engine.hpp"
#include "html-tree-walk.hpp"
#include "lexbor-utils.hpp"

namespace aeronet {

namespace {

using internal::HtmlNodeAccess;

lxb_dom_node_t* Parse(std::string_view html, std::string_view contentType, const lxb_encoding_data_t*& pEncoding) {
  RawChars buffer;
  const internal::Utf8Html utf8 = internal::DecodeHtml(html, contentType, buffer);
  std::unique_ptr<lxb_html_document_t, decltype(&lxb_html_document_destroy)> pDocument(
      internal::CheckCreated(lxb_html_document_create()), &lxb_html_document_destroy);
  // The nodes copy their data: the input can be released after parsing.
  internal::ParseHtml(pDocument.get(), utf8.html);
  pEncoding = utf8.pEncoding;
  return &pDocument.release()->dom_document.node;
}

// The document node is the first member of the HTML document.
lxb_html_document_t* AsHtmlDocument(lxb_dom_node_t* pNode) noexcept {
  return reinterpret_cast<lxb_html_document_t*>(pNode);
}

void Destroy(lxb_dom_node_t* pNode) noexcept {
  if (pNode != nullptr) {
    internal::HtmlQueryEngine::Destroy(pNode);
    lxb_html_document_destroy(AsHtmlDocument(pNode));
  }
}

}  // namespace

HtmlDocument::HtmlDocument(std::string_view html, std::string_view contentType) {
  _pNode = Parse(html, contentType, _pEncoding);
}

HtmlDocument::HtmlDocument(HtmlDocument&& other) noexcept
    : HtmlParentNode(std::exchange(other._pNode, nullptr)), _pEncoding(other._pEncoding) {}

HtmlDocument& HtmlDocument::operator=(HtmlDocument&& other) noexcept {
  if (this != &other) {
    Destroy(_pNode);
    _pNode = std::exchange(other._pNode, nullptr);
    _pEncoding = other._pEncoding;
  }
  return *this;
}

HtmlDocument::~HtmlDocument() { Destroy(_pNode); }

std::string_view HtmlDocument::encoding() const noexcept { return reinterpret_cast<const char*>(_pEncoding->name); }

// The element interfaces start with their node.
HtmlElement HtmlDocument::documentElement() const noexcept {
  return HtmlNodeAccess::Element(
      reinterpret_cast<lxb_dom_node_t*>(lxb_dom_document_element(&AsHtmlDocument(_pNode)->dom_document)));
}

HtmlElement HtmlDocument::head() const noexcept {
  return HtmlNodeAccess::Element(
      reinterpret_cast<lxb_dom_node_t*>(lxb_html_document_head_element(AsHtmlDocument(_pNode))));
}

// The body of lexbor is null in a frameset document, and is the removed <body> when a <frameset> replaced it.
// The parser always creates one of them as a child of <html>, where it only inserts HTML elements.
HtmlElement HtmlDocument::body() const noexcept {
  lxb_dom_node_t* pChild =
      reinterpret_cast<lxb_dom_node_t*>(lxb_dom_document_element(&AsHtmlDocument(_pNode)->dom_document))->first_child;
  while (pChild->local_name != LXB_TAG_BODY && pChild->local_name != LXB_TAG_FRAMESET) {
    pChild = pChild->next;
    assert(pChild != nullptr);
  }
  return HtmlNodeAccess::Element(pChild);
}

std::string_view HtmlDocument::assignedJson(std::string_view variable) const noexcept {
  std::string_view json;
  internal::WalkDescendants(_pNode, [variable, &json](lxb_dom_node_t* pNode) {
    // The text of a <script> element is a single text node: the parser appends adjacent characters to it.
    if (internal::IsTextNode(pNode) && pNode->parent->local_name == LXB_TAG_SCRIPT) {
      const lexbor_str_t& data = reinterpret_cast<lxb_dom_character_data_t*>(pNode)->data;
      json =
          internal::FindAssignedJson(std::string_view(reinterpret_cast<const char*>(data.data), data.length), variable);
    }
    return json.empty();
  });
  return json;
}

}  // namespace aeronet
