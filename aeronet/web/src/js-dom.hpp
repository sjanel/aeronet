#pragma once

#include <quickjs.h>

#include <cstddef>
#include <string_view>

struct lxb_dom_node;

namespace aeronet::internal {

class Page;

// Registers the classes of the node wrappers and defines the DOM interfaces on the global object (Node, Element,
// HTMLElement, CharacterData, Text, Comment, Document, DocumentFragment, SVGElement, with their prototypes), and the
// global 'document'. Returns false on memory or stack exhaustion, the exception pending. Throws std::bad_alloc on
// memory exhaustion of the C++ code.
[[nodiscard]] bool InstallDom(Page& page);

// The node of the wrapper 'value', null when it is not one.
[[nodiscard]] lxb_dom_node* NodeOf(JSContext* pCtx, JSValueConst value) noexcept;

// replaceAll(parent, node), a native helper of the prelude for ParentNode.replaceChildren(): the DOM "replace all" of
// the children of the node 'parent' by 'node', after checking that 'node' can be inserted there (nothing removed
// otherwise, the HierarchyRequestError thrown). Throws a TypeError when 'parent' or 'node' is not a node.
JSValue ReplaceAll(JSContext* pCtx, JSValueConst self, int argc, JSValueConst* argv);

// The HTML element interfaces (HTMLMediaElement, HTMLAnchorElement...), installed on the first lookup of their name on
// the window (see InstallWindow()). Their elements have the prototype of HTMLElement: an interface recognizes them by
// its Symbol.hasInstance.
inline constexpr std::size_t kNbHtmlInterfaces = 35;

// The index of the HTML element interface named 'name', kNbHtmlInterfaces if none.
[[nodiscard]] std::size_t FindHtmlInterface(std::string_view name) noexcept;

// The name of the HTML element interface 'index' ("HTMLMediaElement"...), null-terminated.
[[nodiscard]] const char* HtmlInterfaceName(std::size_t index) noexcept;

// The constructor of the HTML element interface 'index', with its prototype (inheriting from that of its parent,
// HTMLMediaElement for the audio and video ones, looked up first on the object of the lazy globals), for the caller to
// define it once. Returns it, owned, or JS_EXCEPTION on memory exhaustion: nothing is defined then.
[[nodiscard]] JSValue InstallHtmlInterface(Page& page, std::size_t index);

}  // namespace aeronet::internal
