#pragma once

#include <quickjs.h>

#include <cstdint>

struct lxb_dom_node;

namespace aeronet::internal {

class Page;

// The classes of the node wrappers, consecutive from Page::firstNodeClass(), each with the prototype of its interface.
enum class NodeClass : uint8_t { Node, Element, HtmlElement, Text, Comment, Document, Count };

// Registers the classes of the node wrappers and defines the DOM interfaces on the global object (Node, Element,
// HTMLElement, CharacterData, Text, Comment, Document, with their prototypes), and the global 'document'. Returns false
// on memory or stack exhaustion, the exception pending. Throws std::bad_alloc on memory exhaustion of the C++ code.
[[nodiscard]] bool InstallDom(Page& page);

// The class of the wrapper of pNode, not the document node (its wrapper is created by InstallDom()).
NodeClass NodeClassOf(const lxb_dom_node* pNode) noexcept;

// The node of the wrapper 'value', null when it is not one.
[[nodiscard]] lxb_dom_node* NodeOf(JSContext* pCtx, JSValueConst value) noexcept;

}  // namespace aeronet::internal
