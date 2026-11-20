// The DOM bindings of a page: wrappers of the lexbor nodes of its HtmlDocument, whose mutations go through HtmlElement
// and the node operations of aeronet_html (html-node-ops.hpp). Each node has one wrapper, cached in the user pointer of
// its lexbor node (the one of the document node holds the query engine of HtmlDocument: the document wrapper is the
// first one kept by the page) and kept until the page dies.

#include "js-dom.hpp"

#include <lexbor/core/base.h>
#include <lexbor/core/str.h>
#include <lexbor/dom/interfaces/character_data.h>
#include <lexbor/dom/interfaces/node.h>
#include <lexbor/ns/const.h>
#include <lexbor/tag/const.h>
#include <quickjs.h>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "aeronet/ascii-whitespace-tokens-iterator.hpp"
#include "aeronet/cookie-jar.hpp"
#include "aeronet/html-document.hpp"
#include "aeronet/html-element.hpp"
#include "aeronet/small-vector.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "html-node-access.hpp"
#include "html-node-ops.hpp"
#include "html-tree-walk.hpp"
#include "js-engine.hpp"
#include "page.hpp"

namespace aeronet::internal {

namespace {

constexpr unsigned kNbNodeClasses = static_cast<unsigned>(NodeClass::Count);

// The JS_PROP_* flags are distinct bits of signed int macros: added rather than or-ed (bugprone-signed-bitwise).
constexpr int kWritableConfigurable = JS_PROP_WRITABLE + JS_PROP_CONFIGURABLE;

HtmlElement Element(lxb_dom_node_t* pNode) noexcept { return HtmlNodeAccess::Element(pNode); }

std::string_view Data(const lxb_dom_node_t* pNode) noexcept {
  // The character data interfaces start with their node.
  const lexbor_str_t& data = reinterpret_cast<const lxb_dom_character_data_t*>(pNode)->data;
  return {reinterpret_cast<const char*>(data.data), data.length};
}

bool IsCharacterData(const lxb_dom_node_t* pNode) noexcept {
  return IsTextNode(pNode) || pNode->type == LXB_DOM_NODE_TYPE_COMMENT;
}

bool IsElement(const lxb_dom_node_t* pNode) noexcept { return pNode->type == LXB_DOM_NODE_TYPE_ELEMENT; }

// The interfaces whose bindings run on a node: the nodes they accept as 'this'.
enum class On : uint8_t { Node, Element, CharacterData, ParentNode, Document };

// Whether pNode, of the page of pCtx, implements the interface kOn.
template <On kOn>
bool Implements(JSContext* pCtx, const lxb_dom_node_t* pNode) noexcept {
  if constexpr (kOn == On::Element) {
    return IsElement(pNode);
  } else if constexpr (kOn == On::CharacterData) {
    return IsCharacterData(pNode);
  } else if constexpr (kOn == On::ParentNode) {
    return IsElement(pNode) || pNode == Page::Of(pCtx).documentNode();
  } else if constexpr (kOn == On::Document) {
    return pNode == Page::Of(pCtx).documentNode();
  } else {
    return true;
  }
}

// The node of 'self' when it implements kOn, else null with the TypeError of an illegal invocation thrown.
template <On kOn>
lxb_dom_node_t* NodeOn(JSContext* pCtx, JSValueConst self) noexcept {
  lxb_dom_node_t* pNode = NodeOf(pCtx, self);
  if (pNode == nullptr || !Implements<kOn>(pCtx, pNode)) [[unlikely]] {
    ThrowIllegalInvocation(pCtx);
    return nullptr;
  }
  return pNode;
}

// The bindings of an interface take the node of 'this', checked by the native functions that run them.
using NodeGetterFn = JSValue (*)(JSContext*, lxb_dom_node_t*);
using NodeSetterFn = JSValue (*)(JSContext*, lxb_dom_node_t*, JSValueConst);
using NodeMethodFn = JSValue (*)(JSContext*, lxb_dom_node_t*, int, JSValueConst*);

template <On kOn, NodeGetterFn Fn>
JSValue NodeGetter(JSContext* pCtx, JSValueConst self) {
  lxb_dom_node_t* pNode = NodeOn<kOn>(pCtx, self);
  return pNode == nullptr ? JS_EXCEPTION : Fn(pCtx, pNode);
}

template <On kOn, NodeSetterFn Fn>
JSValue NodeSetter(JSContext* pCtx, JSValueConst self, JSValueConst value) {
  lxb_dom_node_t* pNode = NodeOn<kOn>(pCtx, self);
  return pNode == nullptr ? JS_EXCEPTION : Fn(pCtx, pNode, value);
}

template <On kOn, NodeMethodFn Fn>
JSValue NodeMethod(JSContext* pCtx, JSValueConst self, int argc, JSValueConst* argv) {
  lxb_dom_node_t* pNode = NodeOn<kOn>(pCtx, self);
  return pNode == nullptr ? JS_EXCEPTION : Fn(pCtx, pNode, argc, argv);
}

// The entries of the function lists of the interfaces.
template <On kOn, NodeGetterFn Get>
consteval JSCFunctionListEntry Getter(const char* name) noexcept {
  return JS_CGETSET_DEF(name, (&JsGetter<NodeGetter<kOn, Get>>), nullptr);
}

template <On kOn, NodeGetterFn Get, NodeSetterFn Set>
consteval JSCFunctionListEntry Accessor(const char* name) noexcept {
  return JS_CGETSET_DEF(name, (&JsGetter<NodeGetter<kOn, Get>>), (&JsSetter<NodeSetter<kOn, Set>>));
}

template <On kOn, NodeMethodFn Fn>
consteval JSCFunctionListEntry Method(const char* name, uint8_t length) noexcept {
  return JS_CFUNC_DEF(name, length, (&JsMethod<NodeMethod<kOn, Fn>>));
}

// The TypeError of a call of 'function' with fewer than 'count' arguments. Returns JS_EXCEPTION.
JSValue ThrowMissingArguments(JSContext* pCtx, const char* function, int count) noexcept {
  return JS_ThrowTypeError(pCtx, "%s: %d argument(s) required", function, count);
}

// argv[index], a required argument of 'function': JS_EXCEPTION, with a TypeError thrown, when it is missing.
JSValueConst RequiredArgument(JSContext* pCtx, int argc, JSValueConst* argv, int index, const char* function) noexcept {
  return index < argc ? argv[index] : ThrowMissingArguments(pCtx, function, index + 1);
}

// The node argument argv[index] of 'function': null, with a TypeError thrown, when it is missing or not a node.
lxb_dom_node_t* NodeArgument(JSContext* pCtx, int argc, JSValueConst* argv, int index, const char* function) noexcept {
  if (index >= argc) {
    ThrowMissingArguments(pCtx, function, index + 1);
    return nullptr;
  }
  lxb_dom_node_t* pNode = NodeOf(pCtx, argv[index]);
  if (pNode == nullptr) {
    JS_ThrowTypeError(pCtx, "%s: parameter %d is not of type 'Node'", function, index + 1);
  }
  return pNode;
}

// A DOMString argument: null converts to "null", as in the DOM. Fails with a TypeError when it is missing.
class StringArgument : public JsString {
 public:
  StringArgument(JSContext* pCtx, int argc, JSValueConst* argv, int index, const char* function) noexcept
      : JsString(pCtx, RequiredArgument(pCtx, argc, argv, index, function)) {}
};

// The text of a nullable DOMString setter: null is the empty string (textContent, innerHTML...).
class NullableString : public JsString {
 public:
  NullableString(JSContext* pCtx, JSValueConst value) noexcept : JsString(pCtx, value, NullIsEmpty{}) {}
};

// A JavaScript array, filled with owned values. Defining its elements runs no script (no setter of Array.prototype),
// so that the tree cannot change while it is filled. A failure (of its creation, or of an element) makes it failed for
// good, the exception pending.
class JsArray {
 public:
  explicit JsArray(JSContext* pCtx) noexcept : _array(pCtx, JS_NewArray(pCtx)) {}

  [[nodiscard]] JSContext* context() const noexcept { return _array.context(); }

  // Appends 'value' (owned, freed on failure too), which can be the JS_EXCEPTION of a failed creation. Returns false
  // when the array failed.
  [[nodiscard]] bool push(JSValue value) noexcept {
    JSContext* pCtx = _array.context();
    if (JS_IsException(_array.get())) {
      JS_FreeValue(pCtx, value);
      return false;
    }
    // JS_DefinePropertyValueUint32 frees the value on its failure too (an exception has nothing to free).
    if (JS_IsException(value) || JS_DefinePropertyValueUint32(pCtx, _array.get(), _size++, value, kElementFlags) < 0) {
      _array = JsValue(pCtx, JS_EXCEPTION);
      return false;
    }
    return true;
  }

  // The array, JS_EXCEPTION if it failed.
  JSValue release() noexcept { return _array.release(); }

 private:
  // The JS_PROP_* flags are distinct bits of signed int macros: added rather than or-ed (bugprone-signed-bitwise).
  static constexpr int kElementFlags = JS_PROP_CONFIGURABLE + JS_PROP_WRITABLE + JS_PROP_ENUMERABLE;

  JsValue _array;
  uint32_t _size{};
};

// Upper-case for the HTML elements of an HTML document.
JSValue TagName(JSContext* pCtx, lxb_dom_node_t* pNode) { return NewString(pCtx, DomTagName(pNode)); }

// The number of UTF-16 code units of the UTF-8 'str' (the length of a JavaScript string): one per code point, two for a
// code point beyond U+FFFF, which takes 4 bytes.
uint32_t Utf16Length(std::string_view str) noexcept {
  uint32_t length = 0;
  for (const char ch : str) {
    const auto byte = static_cast<unsigned char>(ch);
    length += static_cast<uint32_t>((byte & 0xC0U) != 0x80U) + static_cast<uint32_t>(byte >= 0xF0U);
  }
  return length;
}

// --- Node ---

JSValue IllegalConstructor(JSContext* pCtx, JSValueConst /*self*/, int /*argc*/, JSValueConst* /*argv*/) noexcept {
  return JS_ThrowTypeError(pCtx, "Illegal constructor");
}

JSValue GetNodeType(JSContext* pCtx, lxb_dom_node_t* pNode) {
  return JS_NewInt32(pCtx, static_cast<int32_t>(pNode->type));
}

JSValue GetNodeName(JSContext* pCtx, lxb_dom_node_t* pNode) {
  switch (pNode->type) {
    case LXB_DOM_NODE_TYPE_ELEMENT:
      return TagName(pCtx, pNode);
    case LXB_DOM_NODE_TYPE_TEXT:
      return NewString(pCtx, "#text");
    case LXB_DOM_NODE_TYPE_COMMENT:
      return NewString(pCtx, "#comment");
    case LXB_DOM_NODE_TYPE_DOCUMENT:
      return NewString(pCtx, "#document");
    case LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT:
      return NewString(pCtx, "#document-fragment");
    default:
      // The doctype of an HTML document. The HTML parser makes no CDATA section nodes (text in foreign content).
      assert(pNode->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE);
      return NewString(pCtx, "html");
  }
}

JSValue GetParentNode(JSContext* pCtx, lxb_dom_node_t* pNode) { return Page::Of(pCtx).wrap(pNode->parent); }

JSValue GetParentElement(JSContext* pCtx, lxb_dom_node_t* pNode) {
  lxb_dom_node_t* pParent = pNode->parent;
  return pParent != nullptr && IsElement(pParent) ? Page::Of(pCtx).wrap(pParent) : JS_NULL;
}

JSValue GetChildNodes(JSContext* pCtx, lxb_dom_node_t* pNode) {
  Page& page = Page::Of(pCtx);
  JsArray children(pCtx);
  for (lxb_dom_node_t* pChild = pNode->first_child; pChild != nullptr; pChild = pChild->next) {
    if (!children.push(page.wrap(pChild))) {
      break;
    }
  }
  return children.release();
}

template <lxb_dom_node_t* lxb_dom_node_t::* kLink>
JSValue GetLinkedNode(JSContext* pCtx, lxb_dom_node_t* pNode) {
  return Page::Of(pCtx).wrap(pNode->*kLink);
}

JSValue GetOwnerDocument(JSContext* pCtx, lxb_dom_node_t* pNode) {
  Page& page = Page::Of(pCtx);
  return pNode == page.documentNode() ? JS_NULL : page.wrap(page.documentNode());
}

JSValue GetIsConnected(JSContext* pCtx, lxb_dom_node_t* pNode) {
  while (pNode->parent != nullptr) {
    pNode = pNode->parent;
  }
  return JS_NewBool(pCtx, pNode == Page::Of(pCtx).documentNode());
}

// Replaces the data of pNode, a character data node, counting its size change in the native memory of the page.
// Returns JS_UNDEFINED, or JS_EXCEPTION with an out of memory error thrown when the page cannot hold it.
JSValue SetCharacterData(JSContext* pCtx, lxb_dom_node_t* pNode, std::string_view data) {
  Page& page = Page::Of(pCtx);
  const std::size_t size = Data(pNode).size();
  const std::size_t growth = data.size() > size ? data.size() - size : 0;
  if (!page.memory().canHold(growth)) {
    return JS_ThrowOutOfMemory(pCtx);
  }

  SetNodeData(pNode, data);

  page.touchDom();
  page.memory().charge(growth);
  if (data.size() < size) {
    page.memory().release(size - data.size());
  }
  return JS_UNDEFINED;
}

JSValue GetTextContent(JSContext* pCtx, lxb_dom_node_t* pNode) {
  if (IsCharacterData(pNode)) {
    return NewString(pCtx, Data(pNode));
  }
  if (!IsElement(pNode)) {
    return pNode->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT ? NewString(pCtx, {}) : JS_NULL;
  }
  return NewString(pCtx, Element(pNode).textContent(Page::Of(pCtx).htmlBuffer()));
}

JSValue SetTextContent(JSContext* pCtx, lxb_dom_node_t* pNode, JSValueConst value) {
  const NullableString text(pCtx, value);
  if (text.failed()) {
    return JS_EXCEPTION;
  }
  // No effect on the document and the doctype.
  if (IsCharacterData(pNode)) {
    return SetCharacterData(pCtx, pNode, text.view());
  }
  if (IsElement(pNode)) {
    // The former children stay in the document (see HtmlElement): the text node is new memory.
    const std::size_t memory = text.view().empty() ? 0 : kNodeMemory + text.view().size();
    Page& page = Page::Of(pCtx);
    if (!page.memory().canHold(memory)) {
      return JS_ThrowOutOfMemory(pCtx);
    }
    Element(pNode).setTextContent(text.view());
    page.touchDom();
    page.memory().charge(memory);
  }
  return JS_UNDEFINED;
}

JSValue GetNodeValue(JSContext* pCtx, lxb_dom_node_t* pNode) {
  return IsCharacterData(pNode) ? NewString(pCtx, Data(pNode)) : JS_NULL;
}

JSValue SetNodeValue(JSContext* pCtx, lxb_dom_node_t* pNode, JSValueConst value) {
  const NullableString data(pCtx, value);
  if (data.failed()) {
    return JS_EXCEPTION;
  }
  return IsCharacterData(pNode) ? SetCharacterData(pCtx, pNode, data.view()) : JS_UNDEFINED;
}

// Whether a child of pParent other than pExcluded (can be null) is of 'type'.
bool HasChildOfType(const lxb_dom_node_t* pParent, lxb_dom_node_type_t type, const lxb_dom_node_t* pExcluded) noexcept {
  for (const lxb_dom_node_t* pChild = pParent->first_child; pChild != nullptr; pChild = pChild->next) {
    if (pChild->type == type && pChild != pExcluded) {
      return true;
    }
  }
  return false;
}

// Whether pNode (can be null) or one of its siblings in the direction of kNext is of 'type'.
template <lxb_dom_node_t* lxb_dom_node_t::* kNext>
bool IsOrHasSiblingOfType(const lxb_dom_node_t* pNode, lxb_dom_node_type_t type) noexcept {
  for (; pNode != nullptr; pNode = pNode->*kNext) {
    if (pNode->type == type) {
      return true;
    }
  }
  return false;
}

// The reason why pNode, an element, a document fragment or a doctype, cannot become a child of the document pDocument
// before pChild (null: after its last child), or in place of pChild when 'replace', null if it can: a document has at
// most one element and one doctype, the doctype first (step 6 of the DOM "ensure pre-insertion validity", and of
// "replace a child").
const char* DocumentChildError(const lxb_dom_node_t* pDocument, const lxb_dom_node_t* pNode,
                               const lxb_dom_node_t* pChild, bool replace) noexcept {
  const lxb_dom_node_t* pReplaced = replace ? pChild : nullptr;
  if (pNode->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE) {
    const bool elementBefore =
        pChild != nullptr ? IsOrHasSiblingOfType<&lxb_dom_node_t::prev>(pChild->prev, LXB_DOM_NODE_TYPE_ELEMENT)
                          : HasChildOfType(pDocument, LXB_DOM_NODE_TYPE_ELEMENT, nullptr);
    return HasChildOfType(pDocument, LXB_DOM_NODE_TYPE_DOCUMENT_TYPE, pReplaced) || elementBefore
               ? "A document has one doctype, before its element"
               : nullptr;
  }
  if (pNode->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT) {
    unsigned elements = 0;
    for (const lxb_dom_node_t* pFragmentChild = pNode->first_child; pFragmentChild != nullptr;
         pFragmentChild = pFragmentChild->next) {
      if (IsTextNode(pFragmentChild)) {
        return "A document cannot have a text child";
      }
      elements += static_cast<unsigned>(IsElement(pFragmentChild));
    }
    if (elements == 0) {
      return nullptr;
    }
    if (elements > 1) {
      return "A document has one element";
    }
  }
  // An element, or the one of a fragment. The doctype must stay before the insertion point.
  const lxb_dom_node_t* pAfter = pChild;
  if (replace && pChild != nullptr) {
    pAfter = pChild->next;
  }
  return HasChildOfType(pDocument, LXB_DOM_NODE_TYPE_ELEMENT, pReplaced) ||
                 IsOrHasSiblingOfType<&lxb_dom_node_t::next>(pAfter, LXB_DOM_NODE_TYPE_DOCUMENT_TYPE)
             ? "A document has one element, after its doctype"
             : nullptr;
}

// The reason why pNode cannot be inserted among the children of pParent before pChild (null: after the last one), or
// in place of pChild when 'replace', null if it can: the DOM "ensure pre-insertion validity" (and its variant of
// "replace a child") for an HTML document, the parent of pChild being checked by the callers.
const char* InsertionError(const lxb_dom_node_t* pParent, const lxb_dom_node_t* pNode, const lxb_dom_node_t* pChild,
                           bool replace, const lxb_dom_node_t* pDocument) noexcept {
  if (!IsElement(pParent) && pParent != pDocument && pParent->type != LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT) {
    return "This node type cannot have children";
  }
  for (const lxb_dom_node_t* pAncestor = pParent; pAncestor != nullptr; pAncestor = pAncestor->parent) {
    if (pAncestor == pNode) {
      return "The new child is an inclusive ancestor of the parent";
    }
  }
  const bool doctype = pNode->type == LXB_DOM_NODE_TYPE_DOCUMENT_TYPE;
  if (pNode->type == LXB_DOM_NODE_TYPE_DOCUMENT || (pParent == pDocument && IsTextNode(pNode)) ||
      (pParent != pDocument && doctype)) {
    return "This node cannot be inserted here";
  }
  if (pParent == pDocument && (doctype || IsElement(pNode) || pNode->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT)) {
    return DocumentChildError(pDocument, pNode, pChild, replace);
  }
  return nullptr;
}

// Whether pNode can be inserted among the children of pParent before pChild, or in place of pChild when 'replace' (see
// InsertionError()): if not, throws the HierarchyRequestError of the reason.
bool CheckInsertion(JSContext* pCtx, const lxb_dom_node_t* pParent, const lxb_dom_node_t* pNode,
                    const lxb_dom_node_t* pChild, bool replace) noexcept {
  const char* pError = InsertionError(pParent, pNode, pChild, replace, Page::Of(pCtx).documentNode());
  if (pError != nullptr) {
    JS_ThrowDOMException(pCtx, "HierarchyRequestError", "%s", pError);
  }
  return pError == nullptr;
}

// Moves pNode (the children of a document fragment), which can be inserted there, before pReference among the children
// of pParent, after the last one if pReference is null.
void Move(Page& page, lxb_dom_node_t* pParent, lxb_dom_node_t* pNode, lxb_dom_node_t* pReference) noexcept {
  if (pNode->type == LXB_DOM_NODE_TYPE_DOCUMENT_FRAGMENT) {
    while (lxb_dom_node_t* pChild = pNode->first_child) {
      RemoveNode(pChild);
      InsertNode(pParent, pChild, pReference);
    }
  } else {
    if (pReference == pNode) {
      pReference = pNode->next;  // inserted before itself: before its next sibling once removed
    }
    if (pNode->parent != nullptr) {
      RemoveNode(pNode);
    }
    InsertNode(pParent, pNode, pReference);
  }
  page.touchDom();
}

// Move() after CheckInsertion() (DOM "pre-insert"). Returns false, nothing moved, when pNode cannot be inserted there.
bool Insert(JSContext* pCtx, lxb_dom_node_t* pParent, lxb_dom_node_t* pNode, lxb_dom_node_t* pReference) noexcept {
  if (!CheckInsertion(pCtx, pParent, pNode, pReference, false)) {
    return false;
  }
  Move(Page::Of(pCtx), pParent, pNode, pReference);
  return true;
}

// The node argument argv[index] of 'function', a child of pParent: null, with the error thrown, when it is not one.
lxb_dom_node_t* ChildArgument(JSContext* pCtx, int argc, JSValueConst* argv, int index, const lxb_dom_node_t* pParent,
                              const char* function) noexcept {
  lxb_dom_node_t* pChild = NodeArgument(pCtx, argc, argv, index, function);
  if (pChild != nullptr && pChild->parent != pParent) {
    JS_ThrowDOMException(pCtx, "NotFoundError", "%s: the node is not a child of this node", function);
    return nullptr;
  }
  return pChild;
}

JSValue AppendChild(JSContext* pCtx, lxb_dom_node_t* pParent, int argc, JSValueConst* argv) {
  lxb_dom_node_t* pNode = NodeArgument(pCtx, argc, argv, 0, "appendChild");
  return pNode != nullptr && Insert(pCtx, pParent, pNode, nullptr) ? JS_DupValue(pCtx, argv[0]) : JS_EXCEPTION;
}

JSValue InsertBefore(JSContext* pCtx, lxb_dom_node_t* pParent, int argc, JSValueConst* argv) {
  lxb_dom_node_t* pNode = NodeArgument(pCtx, argc, argv, 0, "insertBefore");
  if (pNode == nullptr) {
    return JS_EXCEPTION;
  }
  if (argc < 2) {
    return ThrowMissingArguments(pCtx, "insertBefore", 2);
  }
  lxb_dom_node_t* pReference = nullptr;
  if (!JS_IsNull(argv[1]) && !JS_IsUndefined(argv[1])) {
    pReference = ChildArgument(pCtx, argc, argv, 1, pParent, "insertBefore");
    if (pReference == nullptr) {
      return JS_EXCEPTION;
    }
  }
  return Insert(pCtx, pParent, pNode, pReference) ? JS_DupValue(pCtx, argv[0]) : JS_EXCEPTION;
}

JSValue RemoveChild(JSContext* pCtx, lxb_dom_node_t* pParent, int argc, JSValueConst* argv) {
  lxb_dom_node_t* pChild = ChildArgument(pCtx, argc, argv, 0, pParent, "removeChild");
  if (pChild == nullptr) {
    return JS_EXCEPTION;
  }
  RemoveNode(pChild);
  Page::Of(pCtx).touchDom();
  return JS_DupValue(pCtx, argv[0]);
}

JSValue ReplaceChild(JSContext* pCtx, lxb_dom_node_t* pParent, int argc, JSValueConst* argv) {
  lxb_dom_node_t* pNode = NodeArgument(pCtx, argc, argv, 0, "replaceChild");
  if (pNode == nullptr) {
    return JS_EXCEPTION;
  }
  lxb_dom_node_t* pChild = ChildArgument(pCtx, argc, argv, 1, pParent, "replaceChild");
  if (pChild == nullptr || !CheckInsertion(pCtx, pParent, pNode, pChild, true)) {
    return JS_EXCEPTION;
  }
  if (pNode != pChild) {
    // Removing a child of pParent keeps the insertion valid: it is not an ancestor of pParent.
    lxb_dom_node_t* pReference = pChild->next == pNode ? pNode->next : pChild->next;
    RemoveNode(pChild);
    Move(Page::Of(pCtx), pParent, pNode, pReference);
  }
  return JS_DupValue(pCtx, argv[1]);
}

JSValue HasChildNodes(JSContext* pCtx, lxb_dom_node_t* pNode, int /*argc*/, JSValueConst* /*argv*/) {
  return JS_NewBool(pCtx, pNode->first_child != nullptr);
}

JSValue Contains(JSContext* pCtx, lxb_dom_node_t* pNode, int argc, JSValueConst* argv) {
  if (argc == 0 || JS_IsNull(argv[0])) {
    return JS_FALSE;
  }
  const lxb_dom_node_t* pOther = NodeArgument(pCtx, argc, argv, 0, "contains");
  if (pOther == nullptr) {
    return JS_EXCEPTION;
  }
  for (; pOther != nullptr; pOther = pOther->parent) {
    if (pOther == pNode) {
      return JS_TRUE;
    }
  }
  return JS_FALSE;
}

// remove() of Element and CharacterData (the ChildNode mixin).
JSValue Remove(JSContext* pCtx, lxb_dom_node_t* pNode, int /*argc*/, JSValueConst* /*argv*/) {
  if (pNode->parent != nullptr) {
    RemoveNode(pNode);
    Page::Of(pCtx).touchDom();
  }
  return JS_UNDEFINED;
}

// --- ParentNode (Element and Document) ---

// The first element among pNode and its siblings in the direction of kNext.
template <lxb_dom_node_t* lxb_dom_node_t::* kNext>
lxb_dom_node_t* ElementFrom(lxb_dom_node_t* pNode) noexcept {
  while (pNode != nullptr && !IsElement(pNode)) {
    pNode = pNode->*kNext;
  }
  return pNode;
}

template <lxb_dom_node_t* lxb_dom_node_t::* kNext>
JSValue GetLinkedElement(JSContext* pCtx, lxb_dom_node_t* pNode) {
  return Page::Of(pCtx).wrap(ElementFrom<kNext>(pNode->*kNext));
}

JSValue GetChildren(JSContext* pCtx, lxb_dom_node_t* pNode) {
  Page& page = Page::Of(pCtx);
  JsArray children(pCtx);
  for (lxb_dom_node_t* pChild = ElementFrom<&lxb_dom_node_t::next>(pNode->first_child); pChild != nullptr;
       pChild = ElementFrom<&lxb_dom_node_t::next>(pChild->next)) {
    if (!children.push(page.wrap(pChild))) {
      break;
    }
  }
  return children.release();
}

JSValue GetFirstElementChild(JSContext* pCtx, lxb_dom_node_t* pNode) {
  return Page::Of(pCtx).wrap(ElementFrom<&lxb_dom_node_t::next>(pNode->first_child));
}

JSValue GetLastElementChild(JSContext* pCtx, lxb_dom_node_t* pNode) {
  return Page::Of(pCtx).wrap(ElementFrom<&lxb_dom_node_t::prev>(pNode->last_child));
}

JSValue GetChildElementCount(JSContext* pCtx, lxb_dom_node_t* pNode) {
  int32_t count = 0;
  for (const lxb_dom_node_t* pChild = pNode->first_child; pChild != nullptr; pChild = pChild->next) {
    count += static_cast<int32_t>(IsElement(pChild));
  }
  return JS_NewInt32(pCtx, count);
}

// The SyntaxError of 'selector' given to 'function', not a valid selector list. Returns JS_EXCEPTION.
JSValue ThrowSelectorSyntaxError(JSContext* pCtx, const char* function, std::string_view selector) noexcept {
  // A view of a JavaScript string: shorter than 2^30.
  return JS_ThrowDOMException(pCtx, "SyntaxError", "%s: '%.*s' is not a valid selector", function,
                              static_cast<int>(selector.size()), selector.data());
}

JSValue QuerySelector(JSContext* pCtx, lxb_dom_node_t* pRoot, int argc, JSValueConst* argv) {
  const StringArgument selector(pCtx, argc, argv, 0, "querySelector");
  if (selector.failed()) {
    return JS_EXCEPTION;
  }
  lxb_dom_node_t* pFound = nullptr;
  const auto storeFirst = [](HtmlElement element, void* pFirst) {
    *static_cast<lxb_dom_node_t**>(pFirst) = HtmlNodeAccess::Node(element);
    return false;
  };
  if (!VisitMatches(pRoot, selector.view(), storeFirst, static_cast<void*>(&pFound))) {
    return ThrowSelectorSyntaxError(pCtx, "querySelector", selector.view());
  }
  return Page::Of(pCtx).wrap(pFound);
}

JSValue QuerySelectorAll(JSContext* pCtx, lxb_dom_node_t* pRoot, int argc, JSValueConst* argv) {
  const StringArgument selector(pCtx, argc, argv, 0, "querySelectorAll");
  if (selector.failed()) {
    return JS_EXCEPTION;
  }
  JsArray elements(pCtx);
  // The wrappers are added during the search: no script runs, the tree does not change.
  const auto push = [](HtmlElement element, void* pElements) {
    JsArray& array = *static_cast<JsArray*>(pElements);
    return array.push(Page::Of(array.context()).wrap(HtmlNodeAccess::Node(element)));
  };
  if (!VisitMatches(pRoot, selector.view(), push, &elements)) {
    return ThrowSelectorSyntaxError(pCtx, "querySelectorAll", selector.view());
  }
  return elements.release();
}

// The descendant elements of pRoot (an element or the document) for which pred(lxb_dom_node_t*) is true.
template <class Pred>
JSValue DescendantElements(JSContext* pCtx, lxb_dom_node_t* pRoot, Pred pred) {
  Page& page = Page::Of(pCtx);
  JsArray elements(pCtx);
  WalkDescendants(pRoot, [&](lxb_dom_node_t* pNode) {
    return !IsElement(pNode) || !pred(pNode) || elements.push(page.wrap(pNode));
  });
  return elements.release();
}

JSValue GetElementsByTagName(JSContext* pCtx, lxb_dom_node_t* pRoot, int argc, JSValueConst* argv) {
  const StringArgument name(pCtx, argc, argv, 0, "getElementsByTagName");
  if (name.failed()) {
    return JS_EXCEPTION;
  }
  const std::string_view wanted = name.view();
  return DescendantElements(pCtx, pRoot, [wanted](lxb_dom_node_t* pNode) {
    // The name of the HTML elements, in lower case, is matched with the name in lower case.
    const std::string_view tagName = Element(pNode).tagName();
    return wanted == "*" || (pNode->ns == LXB_NS_HTML ? CaseInsensitiveEqual(tagName, wanted) : tagName == wanted);
  });
}

// Whether the ASCII whitespace separated tokens of 'str' include 'token'.
constexpr bool HasToken(std::string_view str, std::string_view token) noexcept {
  for (AsciiWhitespaceTokensIterator it(str); it.hasNext();) {
    if (it.next() == token) {
      return true;
    }
  }
  return false;
}

// Whether the ASCII whitespace separated tokens of 'str' include all those of 'tokens'.
constexpr bool HasAllTokens(std::string_view str, std::string_view tokens) noexcept {
  for (AsciiWhitespaceTokensIterator it(tokens); it.hasNext();) {
    if (!HasToken(str, it.next())) {
      return false;
    }
  }
  return true;
}

JSValue GetElementsByClassName(JSContext* pCtx, lxb_dom_node_t* pRoot, int argc, JSValueConst* argv) {
  const StringArgument names(pCtx, argc, argv, 0, "getElementsByClassName");
  if (names.failed()) {
    return JS_EXCEPTION;
  }
  const std::string_view wanted = names.view();
  // No class name matches no element.
  const bool hasNames = AsciiWhitespaceTokensIterator(wanted).hasNext();
  return DescendantElements(pCtx, pRoot, [wanted, hasNames](lxb_dom_node_t* pNode) {
    return hasNames && HasAllTokens(Element(pNode).attribute("class"), wanted);
  });
}

// --- Element ---

JSValue GetLocalName(JSContext* pCtx, lxb_dom_node_t* pElement) { return NewString(pCtx, Element(pElement).tagName()); }

template <const char* kName>
JSValue GetReflectedAttribute(JSContext* pCtx, lxb_dom_node_t* pElement) {
  return NewString(pCtx, Element(pElement).attribute(kName));
}

// The memory of the attribute 'name' of 'value'.
constexpr std::size_t AttributeMemory(std::string_view name, std::string_view value) noexcept {
  return kNodeMemory + name.size() + value.size();
}

JSValue SetAttribute(JSContext* pCtx, lxb_dom_node_t* pElement, std::string_view name, std::string_view value) {
  if (!HtmlElement::IsValidAttributeName(name)) {
    return JS_ThrowDOMException(pCtx, "InvalidCharacterError", "Invalid attribute name '%.*s'",
                                static_cast<int>(name.size()), name.data());
  }
  HtmlElement element = Element(pElement);

  // A replaced value only counts its size change.
  const bool replaced = element.hasAttribute(name);
  const std::size_t former = replaced ? element.attribute(name).size() : 0;
  const std::size_t growth = replaced ? value.size() - std::min(value.size(), former) : AttributeMemory(name, value);

  Page& page = Page::Of(pCtx);
  if (!page.memory().canHold(growth)) {
    return JS_ThrowOutOfMemory(pCtx);
  }
  element.setAttribute(name, value);
  page.touchDom();
  page.memory().charge(growth);
  if (replaced && value.size() < former) {
    page.memory().release(former - value.size());
  }
  return JS_UNDEFINED;
}

template <const char* kName>
JSValue SetReflectedAttribute(JSContext* pCtx, lxb_dom_node_t* pElement, JSValueConst value) {
  const JsString converted(pCtx, value);
  return converted.failed() ? JS_EXCEPTION : SetAttribute(pCtx, pElement, kName, converted.view());
}

constexpr char kId[] = "id";
constexpr char kClass[] = "class";
constexpr char kCreateTextNode[] = "createTextNode";
constexpr char kCreateComment[] = "createComment";

JSValue GetAttribute(JSContext* pCtx, lxb_dom_node_t* pElement, int argc, JSValueConst* argv) {
  const StringArgument name(pCtx, argc, argv, 0, "getAttribute");
  if (name.failed()) {
    return JS_EXCEPTION;
  }
  const HtmlElement element = Element(pElement);
  return element.hasAttribute(name.view()) ? NewString(pCtx, element.attribute(name.view())) : JS_NULL;
}

JSValue SetAttributeMethod(JSContext* pCtx, lxb_dom_node_t* pElement, int argc, JSValueConst* argv) {
  const StringArgument name(pCtx, argc, argv, 0, "setAttribute");
  if (name.failed()) {
    return JS_EXCEPTION;
  }
  const StringArgument value(pCtx, argc, argv, 1, "setAttribute");
  return value.failed() ? JS_EXCEPTION : SetAttribute(pCtx, pElement, name.view(), value.view());
}

JSValue RemoveAttribute(JSContext* pCtx, lxb_dom_node_t* pElement, int argc, JSValueConst* argv) {
  const StringArgument name(pCtx, argc, argv, 0, "removeAttribute");
  if (name.failed()) {
    return JS_EXCEPTION;
  }
  HtmlElement element = Element(pElement);
  if (element.hasAttribute(name.view())) {
    const std::size_t memory = AttributeMemory(name.view(), element.attribute(name.view()));
    element.removeAttribute(name.view());
    Page& page = Page::Of(pCtx);
    page.touchDom();
    page.memory().release(memory);  // lexbor reuses the memory of a removed attribute
  }
  return JS_UNDEFINED;
}

JSValue HasAttribute(JSContext* pCtx, lxb_dom_node_t* pElement, int argc, JSValueConst* argv) {
  const StringArgument name(pCtx, argc, argv, 0, "hasAttribute");
  return name.failed() ? JS_EXCEPTION : JS_NewBool(pCtx, Element(pElement).hasAttribute(name.view()));
}

JSValue GetAttributeNames(JSContext* pCtx, lxb_dom_node_t* pElement, int /*argc*/, JSValueConst* /*argv*/) {
  JsArray names(pCtx);
  for (const HtmlAttribute attribute : Element(pElement).attributes()) {
    if (!names.push(NewString(pCtx, attribute.name))) {
      break;
    }
  }
  return names.release();
}

// The serialization by 'append' (HtmlElement::appendInnerHtml / appendOuterHtml) of pElement.
template <void (HtmlElement::*kAppend)(std::string&) const>
JSValue GetSerialization(JSContext* pCtx, lxb_dom_node_t* pElement) {
  std::string& html = Page::Of(pCtx).htmlBuffer();
  html.clear();
  (Element(pElement).*kAppend)(html);
  return NewString(pCtx, html);
}

JSValue SetInnerHtml(JSContext* pCtx, lxb_dom_node_t* pElement, JSValueConst value) {
  const NullableString html(pCtx, value);
  if (html.failed()) {
    return JS_EXCEPTION;
  }
  Page& page = Page::Of(pCtx);
  if (!page.memory().canHold(html.view().size() * kHtmlMemoryPerByte)) {
    return JS_ThrowOutOfMemory(pCtx);
  }
  Element(pElement).setInnerHtml(html.view());
  page.touchDom();
  // The former children stay in the document (see HtmlElement): the new ones are new memory.
  page.memory().charge(NodesMemory(ChildrenParent(pElement)->first_child, nullptr));
  return page.memory().exceeded() ? JS_ThrowOutOfMemory(pCtx) : JS_UNDEFINED;
}

// The position argument argv[0] of 'function', into 'position'. Returns false, with the error thrown, when it is not
// one.
bool PositionArgument(JSContext* pCtx, int argc, JSValueConst* argv, const char* function,
                      HtmlInsertPosition& position) noexcept {
  const StringArgument name(pCtx, argc, argv, 0, function);
  if (name.failed()) {
    return false;
  }
  struct NamedPosition {
    std::string_view name;
    HtmlInsertPosition position;
  };
  static constexpr NamedPosition kPositions[] = {
      {"beforebegin", HtmlInsertPosition::BeforeBegin},
      {"afterbegin", HtmlInsertPosition::AfterBegin},
      {"beforeend", HtmlInsertPosition::BeforeEnd},
      {"afterend", HtmlInsertPosition::AfterEnd},
  };
  for (const NamedPosition& named : kPositions) {
    if (CaseInsensitiveEqual(name.view(), named.name)) {
      position = named.position;
      return true;
    }
  }
  const std::string_view invalid = name.view();
  JS_ThrowDOMException(pCtx, "SyntaxError", "%s: invalid position '%.*s'", function, static_cast<int>(invalid.size()),
                       invalid.data());
  return false;
}

JSValue InsertAdjacentHtml(JSContext* pCtx, lxb_dom_node_t* pElement, int argc, JSValueConst* argv) {
  HtmlInsertPosition position{};
  if (!PositionArgument(pCtx, argc, argv, "insertAdjacentHTML", position)) {
    return JS_EXCEPTION;
  }
  const StringArgument html(pCtx, argc, argv, 1, "insertAdjacentHTML");
  if (html.failed()) {
    return JS_EXCEPTION;
  }
  // HTML insertAdjacentHTML(): beside an element without parent or beside the root element is an error.
  const InsertionPoint point = AdjacentInsertionPoint(pElement, position);
  if (!CanInsertAdjacent(point)) {
    return JS_ThrowDOMException(pCtx, "NoModificationAllowedError",
                                "insertAdjacentHTML: the element has no parent, or is the root element");
  }
  Page& page = Page::Of(pCtx);
  if (!page.memory().canHold(html.view().size() * kHtmlMemoryPerByte)) {
    return JS_ThrowOutOfMemory(pCtx);
  }
  // The inserted nodes go after the node before the insertion point (null: first children of its parent).
  const lxb_dom_node_t* pBefore = point.pReference != nullptr ? point.pReference->prev : point.pParent->last_child;
  Element(pElement).insertAdjacentHtml(position, html.view());
  page.touchDom();
  page.memory().charge(NodesMemory(pBefore != nullptr ? pBefore->next : point.pParent->first_child, point.pReference));
  return page.memory().exceeded() ? JS_ThrowOutOfMemory(pCtx) : JS_UNDEFINED;
}

JSValue InsertAdjacentElement(JSContext* pCtx, lxb_dom_node_t* pElement, int argc, JSValueConst* argv) {
  HtmlInsertPosition position{};
  if (!PositionArgument(pCtx, argc, argv, "insertAdjacentElement", position)) {
    return JS_EXCEPTION;
  }
  lxb_dom_node_t* pOther = NodeArgument(pCtx, argc, argv, 1, "insertAdjacentElement");
  if (pOther == nullptr) {
    return JS_EXCEPTION;
  }
  if (!IsElement(pOther)) {
    return JS_ThrowTypeError(pCtx, "insertAdjacentElement: parameter 2 is not of type 'Element'");
  }
  // DOM "insert adjacent": nothing beside an element without parent, a pre-insertion otherwise (an error beside the
  // root element: the document has it as element).
  const InsertionPoint point = AdjacentInsertionPoint(pElement, position);
  if (point.pParent == nullptr) {
    return JS_NULL;
  }
  return Insert(pCtx, point.pParent, pOther, point.pReference) ? JS_DupValue(pCtx, argv[1]) : JS_EXCEPTION;
}

JSValue InsertAdjacentText(JSContext* pCtx, lxb_dom_node_t* pElement, int argc, JSValueConst* argv) {
  HtmlInsertPosition position{};
  if (!PositionArgument(pCtx, argc, argv, "insertAdjacentText", position)) {
    return JS_EXCEPTION;
  }
  const StringArgument text(pCtx, argc, argv, 1, "insertAdjacentText");
  if (text.failed()) {
    return JS_EXCEPTION;
  }
  // DOM "insert adjacent": nothing beside an element without parent, an error beside the root element (a text node
  // cannot be a child of the document).
  const InsertionPoint point = AdjacentInsertionPoint(pElement, position);
  if (point.pParent == nullptr) {
    return JS_UNDEFINED;
  }
  if (!CanInsertAdjacent(point)) {
    return JS_ThrowDOMException(pCtx, "HierarchyRequestError",
                                "insertAdjacentText: a document cannot have a text child");
  }
  const std::size_t memory = text.view().empty() ? 0 : kNodeMemory + text.view().size();
  Page& page = Page::Of(pCtx);
  if (!page.memory().canHold(memory)) {
    return JS_ThrowOutOfMemory(pCtx);
  }
  Element(pElement).insertAdjacentText(position, text.view());
  page.touchDom();
  page.memory().charge(memory);
  return JS_UNDEFINED;
}

// The contents of a <template> (a document fragment), the reflected attribute of a <meta>, undefined for the other
// elements.
JSValue GetContent(JSContext* pCtx, lxb_dom_node_t* pElement) {
  if (pElement->ns != LXB_NS_HTML) {
    return JS_UNDEFINED;
  }
  if (pElement->local_name == LXB_TAG_TEMPLATE) {
    return Page::Of(pCtx).wrap(ChildrenParent(pElement));
  }
  return pElement->local_name == LXB_TAG_META ? NewString(pCtx, Element(pElement).attribute("content")) : JS_UNDEFINED;
}

// --- CharacterData ---

JSValue GetData(JSContext* pCtx, lxb_dom_node_t* pNode) { return NewString(pCtx, Data(pNode)); }

JSValue SetData(JSContext* pCtx, lxb_dom_node_t* pNode, JSValueConst value) {
  const NullableString data(pCtx, value);
  return data.failed() ? JS_EXCEPTION : SetCharacterData(pCtx, pNode, data.view());
}

JSValue GetLength(JSContext* pCtx, lxb_dom_node_t* pNode) { return JS_NewUint32(pCtx, Utf16Length(Data(pNode))); }

// --- Document ---

template <HtmlElement (HtmlDocument::*kElement)() const noexcept>
JSValue GetDocumentElementOf(JSContext* pCtx, lxb_dom_node_t* /*pDocument*/) {
  Page& page = Page::Of(pCtx);
  return page.wrap(HtmlNodeAccess::Node((page.document().*kElement)()));
}

JSValue GetElementById(JSContext* pCtx, lxb_dom_node_t* pDocument, int argc, JSValueConst* argv) {
  const StringArgument id(pCtx, argc, argv, 0, "getElementById");
  if (id.failed()) {
    return JS_EXCEPTION;
  }
  lxb_dom_node_t* pFound = nullptr;
  if (!id.view().empty()) {
    WalkDescendants(pDocument, [&](lxb_dom_node_t* pNode) {
      if (IsElement(pNode) && Element(pNode).attribute("id") == id.view()) {
        pFound = pNode;
      }
      return pFound == nullptr;
    });
  }
  return Page::Of(pCtx).wrap(pFound);
}

JSValue CreateElement(JSContext* pCtx, lxb_dom_node_t* /*pDocument*/, int argc, JSValueConst* argv) {
  const StringArgument name(pCtx, argc, argv, 0, "createElement");
  if (name.failed()) {
    return JS_EXCEPTION;
  }
  if (!HtmlDocument::IsValidElementName(name.view())) {
    const std::string_view invalid = name.view();
    return JS_ThrowDOMException(pCtx, "InvalidCharacterError", "Invalid element name '%.*s'",
                                static_cast<int>(invalid.size()), invalid.data());
  }
  Page& page = Page::Of(pCtx);
  const std::size_t memory = kNodeMemory + name.view().size();
  if (!page.memory().canHold(memory)) {
    return JS_ThrowOutOfMemory(pCtx);
  }
  lxb_dom_node_t* pElement = HtmlNodeAccess::Node(page.document().createElement(name.view()));
  page.memory().charge(memory);
  return page.wrap(pElement);
}

template <lxb_dom_node_t* (*kCreate)(lxb_dom_node_t*, std::string_view), const char* kFunction>
JSValue CreateCharacterData(JSContext* pCtx, lxb_dom_node_t* pDocument, int argc, JSValueConst* argv) {
  const StringArgument data(pCtx, argc, argv, 0, kFunction);
  if (data.failed()) {
    return JS_EXCEPTION;
  }
  Page& page = Page::Of(pCtx);
  const std::size_t memory = kNodeMemory + data.view().size();
  if (!page.memory().canHold(memory)) {
    return JS_ThrowOutOfMemory(pCtx);
  }
  lxb_dom_node_t* pNode = kCreate(pDocument, data.view());
  page.memory().charge(memory);
  return page.wrap(pNode);
}

JSValue GetReadyState(JSContext* pCtx, lxb_dom_node_t* /*pDocument*/) {
  switch (Page::Of(pCtx).readyState()) {
    case Page::ReadyState::Loading:
      return NewString(pCtx, "loading");
    case Page::ReadyState::Interactive:
      return NewString(pCtx, "interactive");
    default:
      return NewString(pCtx, "complete");
  }
}

JSValue GetCookie(JSContext* pCtx, lxb_dom_node_t* /*pDocument*/) {
  const Page& page = Page::Of(pCtx);
  return NewString(pCtx, page.session().http.cookieJar().documentCookie(page.url()));
}

JSValue SetCookie(JSContext* pCtx, lxb_dom_node_t* /*pDocument*/, JSValueConst value) {
  const JsString cookie(pCtx, value);
  if (cookie.failed()) {
    return JS_EXCEPTION;
  }
  const Page& page = Page::Of(pCtx);
  // A refused cookie is ignored, as browsers do.
  static_cast<void>(page.session().http.cookieJar().setDocumentCookie(page.url(), cookie.view()));
  return JS_UNDEFINED;
}

JSValue GetCurrentScript(JSContext* pCtx, lxb_dom_node_t* /*pDocument*/) {
  Page& page = Page::Of(pCtx);
  return page.wrap(page.currentScript());
}

JSValue GetCharacterSet(JSContext* pCtx, lxb_dom_node_t* /*pDocument*/) {
  return NewString(pCtx, Page::Of(pCtx).document().encoding());
}

// Ignored outside of a script of the parser (see Page::documentWrite()).
template <bool kNewLine>
JSValue Write(JSContext* pCtx, lxb_dom_node_t* /*pDocument*/, int argc, JSValueConst* argv) {
  Page& page = Page::Of(pCtx);
  if (argc == 1) {
    const JsString text(pCtx, argv[0]);
    if (text.failed()) {
      return JS_EXCEPTION;
    }
    return page.documentWrite(text.view(), kNewLine) ? JS_UNDEFINED : JS_ThrowOutOfMemory(pCtx);
  }
  // The arguments are converted first: a conversion can write too (toString()). Short writes stay on the stack.
  static constexpr std::size_t kInlineChars = 256;
  SmallVector<char, kInlineChars> html;
  for (int index = 0; index < argc; ++index) {
    const JsString text(pCtx, argv[index]);
    if (text.failed()) {
      return JS_EXCEPTION;
    }
    html.insert(html.end(), text.view().begin(), text.view().end());
  }
  return page.documentWrite(std::string_view(html.data(), html.size()), kNewLine) ? JS_UNDEFINED
                                                                                  : JS_ThrowOutOfMemory(pCtx);
}

// --- Function lists of the interfaces ---

constexpr JSCFunctionListEntry kNodeFunctions[] = {
    Getter<On::Node, GetNodeType>("nodeType"),
    Getter<On::Node, GetNodeName>("nodeName"),
    Getter<On::Node, GetParentNode>("parentNode"),
    Getter<On::Node, GetParentElement>("parentElement"),
    Getter<On::Node, GetChildNodes>("childNodes"),
    Getter<On::Node, GetLinkedNode<&lxb_dom_node_t::first_child>>("firstChild"),
    Getter<On::Node, GetLinkedNode<&lxb_dom_node_t::last_child>>("lastChild"),
    Getter<On::Node, GetLinkedNode<&lxb_dom_node_t::prev>>("previousSibling"),
    Getter<On::Node, GetLinkedNode<&lxb_dom_node_t::next>>("nextSibling"),
    Getter<On::Node, GetOwnerDocument>("ownerDocument"),
    Getter<On::Node, GetIsConnected>("isConnected"),
    Accessor<On::Node, GetTextContent, SetTextContent>("textContent"),
    Accessor<On::Node, GetNodeValue, SetNodeValue>("nodeValue"),
    Method<On::Node, AppendChild>("appendChild", 1),
    Method<On::Node, InsertBefore>("insertBefore", 2),
    Method<On::Node, RemoveChild>("removeChild", 1),
    Method<On::Node, ReplaceChild>("replaceChild", 2),
    Method<On::Node, HasChildNodes>("hasChildNodes", 0),
    Method<On::Node, Contains>("contains", 1),
};

// The ParentNode mixin, on Element and Document.
constexpr JSCFunctionListEntry kParentNodeFunctions[] = {
    Getter<On::ParentNode, GetChildren>("children"),
    Getter<On::ParentNode, GetFirstElementChild>("firstElementChild"),
    Getter<On::ParentNode, GetLastElementChild>("lastElementChild"),
    Getter<On::ParentNode, GetChildElementCount>("childElementCount"),
    Method<On::ParentNode, QuerySelector>("querySelector", 1),
    Method<On::ParentNode, QuerySelectorAll>("querySelectorAll", 1),
    Method<On::ParentNode, GetElementsByTagName>("getElementsByTagName", 1),
    Method<On::ParentNode, GetElementsByClassName>("getElementsByClassName", 1),
};

constexpr JSCFunctionListEntry kElementFunctions[] = {
    Getter<On::Element, TagName>("tagName"),
    Getter<On::Element, GetLocalName>("localName"),
    Accessor<On::Element, GetReflectedAttribute<kId>, SetReflectedAttribute<kId>>("id"),
    Accessor<On::Element, GetReflectedAttribute<kClass>, SetReflectedAttribute<kClass>>("className"),
    Accessor<On::Element, GetSerialization<&HtmlElement::appendInnerHtml>, SetInnerHtml>("innerHTML"),
    Getter<On::Element, GetSerialization<&HtmlElement::appendOuterHtml>>("outerHTML"),
    Getter<On::Element, GetContent>("content"),
    Getter<On::Node, GetLinkedElement<&lxb_dom_node_t::prev>>("previousElementSibling"),
    Getter<On::Node, GetLinkedElement<&lxb_dom_node_t::next>>("nextElementSibling"),
    Method<On::Element, GetAttribute>("getAttribute", 1),
    Method<On::Element, SetAttributeMethod>("setAttribute", 2),
    Method<On::Element, RemoveAttribute>("removeAttribute", 1),
    Method<On::Element, HasAttribute>("hasAttribute", 1),
    Method<On::Element, GetAttributeNames>("getAttributeNames", 0),
    Method<On::Element, InsertAdjacentHtml>("insertAdjacentHTML", 2),
    Method<On::Element, InsertAdjacentElement>("insertAdjacentElement", 2),
    Method<On::Element, InsertAdjacentText>("insertAdjacentText", 2),
    Method<On::Node, Remove>("remove", 0),
};

constexpr JSCFunctionListEntry kCharacterDataFunctions[] = {
    Accessor<On::CharacterData, GetData, SetData>("data"),
    Getter<On::Node, GetLinkedElement<&lxb_dom_node_t::prev>>("previousElementSibling"),
    Getter<On::Node, GetLinkedElement<&lxb_dom_node_t::next>>("nextElementSibling"),
    Getter<On::CharacterData, GetLength>("length"),
    Method<On::Node, Remove>("remove", 0),
};

constexpr JSCFunctionListEntry kDocumentFunctions[] = {
    Getter<On::Document, GetDocumentElementOf<&HtmlDocument::documentElement>>("documentElement"),
    Getter<On::Document, GetDocumentElementOf<&HtmlDocument::head>>("head"),
    Getter<On::Document, GetDocumentElementOf<&HtmlDocument::body>>("body"),
    Getter<On::Document, GetReadyState>("readyState"),
    Accessor<On::Document, GetCookie, SetCookie>("cookie"),
    Getter<On::Document, GetCurrentScript>("currentScript"),
    Getter<On::Document, GetCharacterSet>("characterSet"),
    Method<On::Document, GetElementById>("getElementById", 1),
    Method<On::Document, CreateElement>("createElement", 1),
    Method<On::Document, CreateCharacterData<&NewTextNode, kCreateTextNode>>("createTextNode", 1),
    Method<On::Document, CreateCharacterData<&NewCommentNode, kCreateComment>>("createComment", 1),
    Method<On::Document, Write<false>>("write", 1),
    Method<On::Document, Write<true>>("writeln", 1),
};

struct Interface {
  const char* name;
  int parent;     // index of the parent interface, -1 for none
  int nodeClass;  // NodeClass of the wrappers whose prototype it is, -1 for none
  std::span<const JSCFunctionListEntry> functions;
  std::span<const JSCFunctionListEntry> mixin;
};

// In order of inheritance.
const Interface kInterfaces[] = {
    {"Node", -1, static_cast<int>(NodeClass::Node), kNodeFunctions, {}},
    {"Element", 0, static_cast<int>(NodeClass::Element), kElementFunctions, kParentNodeFunctions},
    {"HTMLElement", 1, static_cast<int>(NodeClass::HtmlElement), {}, {}},
    {"CharacterData", 0, -1, kCharacterDataFunctions, {}},
    {"Text", 3, static_cast<int>(NodeClass::Text), {}, {}},
    {"Comment", 3, static_cast<int>(NodeClass::Comment), {}, {}},
    {"Document", 0, static_cast<int>(NodeClass::Document), kDocumentFunctions, kParentNodeFunctions},
};

}  // namespace

NodeClass NodeClassOf(const lxb_dom_node_t* pNode) noexcept {
  switch (pNode->type) {
    case LXB_DOM_NODE_TYPE_ELEMENT:
      return pNode->ns == LXB_NS_HTML ? NodeClass::HtmlElement : NodeClass::Element;
    case LXB_DOM_NODE_TYPE_TEXT:
    case LXB_DOM_NODE_TYPE_CDATA_SECTION:
      return NodeClass::Text;
    case LXB_DOM_NODE_TYPE_COMMENT:
      return NodeClass::Comment;
    default:
      // The document wrapper is created with the page (see InstallDom()).
      assert(pNode->type != LXB_DOM_NODE_TYPE_DOCUMENT);
      return NodeClass::Node;
  }
}

lxb_dom_node_t* NodeOf(JSContext* pCtx, JSValueConst value) noexcept {
  JSClassID classId = 0;
  void* pNode = JS_GetAnyOpaque(value, &classId);
  return pNode != nullptr && classId - Page::Of(pCtx).firstNodeClass() < kNbNodeClasses
             ? static_cast<lxb_dom_node_t*>(pNode)
             : nullptr;
}

JSValue Page::wrap(lxb_dom_node_t* pNode) {
  JSContext* pCtx = context();
  if (pNode == nullptr) {
    return JS_NULL;
  }
  if (pNode == _pDocumentNode) {
    return JS_DupValue(pCtx, _wrappers.front());
  }
  if (pNode->user != nullptr) {
    return JS_DupValue(pCtx, JS_MKPTR(JS_TAG_OBJECT, pNode->user));
  }
  JsValue wrapper(pCtx, JS_NewObjectClass(pCtx, (_firstNodeClass + static_cast<JSClassID>(NodeClassOf(pNode)))));
  if (JS_IsException(wrapper.get())) {
    return JS_EXCEPTION;
  }
  SetOpaque(wrapper.get(), pNode);
  keepWrapper(wrapper.get());
  pNode->user = JS_VALUE_GET_PTR(wrapper.get());
  return wrapper.release();
}

bool InstallDom(Page& page) {
  JSContext* pCtx = page.context();

  static constexpr const char* kClassNames[kNbNodeClasses] = {
      "Node", "Element", "HTMLElement", "Text", "Comment", "HTMLDocument",
  };
  for (unsigned index = 0; index < kNbNodeClasses; ++index) {
    const JSClassID classId = NewClass(pCtx, kClassNames[index]);
    if (classId == 0) {
      return false;
    }
    if (index == 0) {
      page.setFirstNodeClass(classId);
    }
    assert(classId == page.firstNodeClass() + index);  // allocated in sequence by the runtime
  }

  const JsValue global(pCtx, JS_GetGlobalObject(pCtx));
  JsValue prototypes[std::size(kInterfaces)];
  for (std::size_t index = 0; index < std::size(kInterfaces); ++index) {
    const Interface& domInterface = kInterfaces[index];
    prototypes[index] =
        JsValue(pCtx, domInterface.parent < 0 ? JS_NewObject(pCtx)
                                              : JS_NewObjectProto(pCtx, prototypes[domInterface.parent].get()));
    const JSValue prototype = prototypes[index].get();
    if (JS_IsException(prototype) || !SetFunctions(pCtx, prototype, domInterface.functions) ||
        !SetFunctions(pCtx, prototype, domInterface.mixin)) {
      return false;
    }
    JsValue constructor(pCtx,
                        JS_NewCFunction2(pCtx, &IllegalConstructor, domInterface.name, 0, JS_CFUNC_constructor, 0));
    if (JS_IsException(constructor.get()) || JS_SetConstructor(pCtx, constructor.get(), prototype) < 0 ||
        !Define(pCtx, global.get(), domInterface.name, constructor.release(), kWritableConfigurable)) {
      return false;
    }
    if (domInterface.nodeClass >= 0) {
      JS_SetClassProto(pCtx, page.firstNodeClass() + static_cast<JSClassID>(domInterface.nodeClass),
                       JS_DupValue(pCtx, prototype));
    }
  }

  // The document wrapper is the first one kept: the user pointer of its node is taken (see Page::wrap()).
  JsValue document(pCtx,
                   JS_NewObjectClass(pCtx, (page.firstNodeClass() + static_cast<JSClassID>(NodeClass::Document))));
  if (JS_IsException(document.get())) {
    return false;
  }
  SetOpaque(document.get(), page.documentNode());
  page.keepWrapper(document.get());
  return Define(pCtx, global.get(), "document", document.release(), JS_PROP_ENUMERABLE);
}

}  // namespace aeronet::internal
