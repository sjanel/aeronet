#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include "aeronet/css-selector.hpp"
#include "aeronet/vector.hpp"

struct lxb_dom_attr;
struct lxb_dom_node;

namespace aeronet {

class HtmlElement;

namespace internal {
struct HtmlNodeAccess;  // gives the implementation access to the lexbor nodes (html-node-access.hpp)

// Called for each element of a search, which goes on while it returns true.
using HtmlElementCallback = bool (*)(HtmlElement element, void* pCallback);
}  // namespace internal

// The selector queries of HtmlDocument and HtmlElement (the ParentNode mixin of the DOM). They search the descendants
// of the node in document order, each matching element once. A selector given as text is parsed at each call and throws
// std::invalid_argument when invalid: compile a CssSelector to reuse one.
// An HtmlDocument and its elements must not be used from several threads at the same time, even through const methods:
// the queries share the matching engine of the document.
class HtmlParentNode {
 public:
  // The first descendant element matching selector, or an invalid HtmlElement if there is none.
  [[nodiscard]] HtmlElement querySelector(std::string_view selector) const;
  [[nodiscard]] HtmlElement querySelector(const CssSelector& selector) const;

  // All the descendant elements matching selector.
  [[nodiscard]] vector<HtmlElement> querySelectorAll(std::string_view selector) const;
  [[nodiscard]] vector<HtmlElement> querySelectorAll(const CssSelector& selector) const;

  // Calls callback(HtmlElement) for each descendant element matching selector, without collecting them. A callback
  // returning a bool stops the search by returning false (its other return types are ignored). An exception thrown by
  // callback stops the search and is propagated. The callback can change attributes, not the tree (setInnerHtml(),
  // setTextContent(), insertions, remove()...): collect the elements with querySelectorAll() first.
  template <class Callback>
    requires std::invocable<Callback&, HtmlElement>
  void forEach(std::string_view selector, Callback&& callback) const {
    forEachMatch(selector, &CallElementCallback<std::remove_reference_t<Callback>>, ErasedPointer(callback));
  }

  template <class Callback>
    requires std::invocable<Callback&, HtmlElement>
  void forEach(const CssSelector& selector, Callback&& callback) const {
    forEachMatch(selector, &CallElementCallback<std::remove_reference_t<Callback>>, ErasedPointer(callback));
  }

 protected:
  HtmlParentNode() noexcept = default;

  explicit HtmlParentNode(lxb_dom_node* pNode) noexcept : _pNode(pNode) {}

  // Only through the derived classes: no slicing of a document into a non-owning node.
  HtmlParentNode(const HtmlParentNode&) = default;
  HtmlParentNode(HtmlParentNode&&) noexcept = default;
  HtmlParentNode& operator=(const HtmlParentNode&) noexcept = default;
  HtmlParentNode& operator=(HtmlParentNode&&) noexcept = default;

  ~HtmlParentNode() = default;

  lxb_dom_node* _pNode{};

 private:
  template <class Fn>
  static bool CallElementCallback(HtmlElement element, void* pCallback);

  // The callback is given back to CallElementCallback with its own constness.
  template <class Fn>
  static void* ErasedPointer(Fn& callback) noexcept {
    return const_cast<void*>(static_cast<const void*>(std::addressof(callback)));
  }

  friend struct internal::HtmlNodeAccess;

  void forEachMatch(std::string_view selector, internal::HtmlElementCallback callback, void* pCallback) const;
  void forEachMatch(const CssSelector& selector, internal::HtmlElementCallback callback, void* pCallback) const;
};

// An attribute of an element. Its views point into the document, valid as long as it lives (the value until
// setAttribute() changes it), even once the attribute is removed.
struct HtmlAttribute {
  std::string_view name;
  std::string_view value;
};

// The attributes of an element, in source order (forward range of HtmlAttribute). The attribute being visited can be
// removed: the iteration goes on with the next one.
class HtmlAttributes {
 public:
  class iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = HtmlAttribute;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = HtmlAttribute;

    iterator() noexcept = default;

    [[nodiscard]] HtmlAttribute operator*() const noexcept;

    iterator& operator++() noexcept;

    iterator operator++(int) noexcept {
      iterator ret = *this;
      ++*this;
      return ret;
    }

    // Two iterators on the same attribute.
    bool operator==(const iterator& other) const noexcept { return _pAttr == other._pAttr; }

   private:
    friend class HtmlAttributes;

    explicit iterator(lxb_dom_attr* pAttr) noexcept;

    lxb_dom_attr* _pAttr{};
    lxb_dom_attr* _pNext{};  // read on arrival at _pAttr, which can then be removed (unlinked from the next one)
  };

  [[nodiscard]] iterator begin() const noexcept { return iterator(_pFirst); }
  static iterator end() noexcept { return {}; }

  [[nodiscard]] bool empty() const noexcept { return _pFirst == nullptr; }

 private:
  friend class HtmlElement;

  explicit HtmlAttributes(lxb_dom_attr* pFirst) noexcept : _pFirst(pFirst) {}

  lxb_dom_attr* _pFirst;
};

class HtmlChildren;

// Where insertAdjacentHtml(), insertAdjacentElement() and insertAdjacentText() insert, relative to an element (the
// "beforebegin", "afterbegin", "beforeend" and "afterend" of the DOM).
enum class HtmlInsertPosition : std::uint8_t {
  BeforeBegin,  // before the element, among the children of its parent
  AfterBegin,   // in the element, before its first child
  BeforeEnd,    // in the element, after its last child
  AfterEnd,     // after the element, among the children of its parent
};

// An element of an HtmlDocument: a non-owning handle (one pointer, cheap to copy), valid as long as its document lives.
// A default-constructed element, or one returned when no element matches, is invalid: only valid() and comparisons can
// be called on it.
// Navigation goes through elements only (no text, comment or document node): parent(), children()...
// The tree can be modified (attributes, text, children): the queries and serializations see the modified tree. An
// element taken out of the tree (by remove(), or replaced by setInnerHtml() or setTextContent()) stays in its document
// until the document is destroyed: handles on it stay valid, and it can be inserted again.
class HtmlElement : public HtmlParentNode {
 public:
  // An invalid element.
  HtmlElement() noexcept = default;

  [[nodiscard]] bool valid() const noexcept { return _pNode != nullptr; }

  explicit operator bool() const noexcept { return valid(); }

  // The name of the element, as the DOM localName: in lower case for HTML elements ("div", "tr"), unlike the upper-case
  // tagName of the DOM, in the case of SVG for SVG elements ("foreignObject", "linearGradient"). An element created
  // with a namespace prefix (only by the scripts of aeronet_web) has its qualified name: "svg:rect". The view points
  // into the document.
  [[nodiscard]] std::string_view tagName() const noexcept;

  // The value of the attribute 'name' (matched in lower case for HTML elements), or an empty view if absent: see
  // hasAttribute() to tell an absent attribute from an empty one. The view points into the document, until
  // setAttribute() changes the attribute.
  [[nodiscard]] std::string_view attribute(std::string_view name) const noexcept;

  [[nodiscard]] bool hasAttribute(std::string_view name) const noexcept;

  [[nodiscard]] HtmlAttributes attributes() const noexcept;

  // The concatenated text of all the descendant text nodes, in document order (DOM textContent: the contents of
  // <script> and <style> elements are included, no whitespace is collapsed).
  [[nodiscard]] std::string textContent() const;

  // The same text without copy when it is in at most one text node (as in <td>6.00815 ZRX</td>): the view then points
  // into the document and 'buffer' is untouched. Otherwise the text replaces the contents of 'buffer' and the view
  // points into it: reuse one buffer for many elements, to allocate only when it grows. Read the returned view, valid
  // as long as the document lives and 'buffer' is not modified.
  [[nodiscard]] std::string_view textContent(std::string& buffer) const;

  // The HTML serialization of the children of the element (DOM innerHTML), of its contents for a <template>.
  [[nodiscard]] std::string innerHtml() const;

  // The HTML serialization of the element and its children (DOM outerHTML).
  [[nodiscard]] std::string outerHtml() const;

  // Appends innerHtml(), or outerHtml(), to 'out'.
  void appendInnerHtml(std::string& out) const;
  void appendOuterHtml(std::string& out) const;

  // The parent element, invalid for the root element (<html>) and for the top element of a subtree out of the tree.
  [[nodiscard]] HtmlElement parent() const noexcept;

  // The first and last child elements, invalid if there are none.
  [[nodiscard]] HtmlElement firstChild() const noexcept;
  [[nodiscard]] HtmlElement lastChild() const noexcept;

  // The next and previous sibling elements, invalid if there are none.
  [[nodiscard]] HtmlElement nextSibling() const noexcept;
  [[nodiscard]] HtmlElement previousSibling() const noexcept;

  // The child elements, as a forward range: for (HtmlElement cell : row.children()) { ... }
  [[nodiscard]] HtmlChildren children() const noexcept;

  // Whether 'name' is a valid attribute name (DOM), as setAttribute() needs: not empty, without ASCII whitespace, NUL,
  // '/', '=' or '>'.
  [[nodiscard]] static bool IsValidAttributeName(std::string_view name) noexcept;

  // Sets the attribute 'name' to 'value', added after the others if absent. The name is lower-cased for HTML elements.
  // Throws std::invalid_argument if 'name' is not a valid attribute name (see IsValidAttributeName()), std::bad_alloc
  // on memory exhaustion.
  void setAttribute(std::string_view name, std::string_view value);

  // Removes the attribute 'name' (matched as by attribute()), if present. It stays in the document: the views of its
  // name and value stay valid, as does an attributes() iteration visiting it.
  void removeAttribute(std::string_view name);

  // Replaces the children of the element by a text node of 'text' (none if empty), as the DOM textContent setter: the
  // text is not parsed, "<b>" stays text (serialized as "&lt;b&gt;"). Throws std::bad_alloc on memory exhaustion, the
  // children being unchanged.
  void setTextContent(std::string_view text);

  // Replaces the children of the element by the nodes parsed from 'html' (UTF-8), as the DOM innerHTML setter: 'html'
  // is parsed as a fragment in the context of the element ("<tr>" is a row in a <tbody>, text in a <textarea>), with
  // the parsing limits of HtmlDocument. Replaces the contents of a <template>. Throws std::bad_alloc on memory
  // exhaustion, the children being unchanged.
  void setInnerHtml(std::string_view html);

  // Inserts the nodes parsed from 'html' (UTF-8) at 'position' (DOM insertAdjacentHTML), parsed as a fragment in the
  // context of the element for AfterBegin and BeforeEnd, of its parent for BeforeBegin and AfterEnd (<body> instead
  // of <html> or of the contents of a <template>), with the parsing limits of HtmlDocument. Throws
  // std::invalid_argument for BeforeBegin and AfterEnd if the element is the root element or has no parent,
  // std::bad_alloc on memory exhaustion (nothing inserted).
  void insertAdjacentHtml(HtmlInsertPosition position, std::string_view html);

  // Moves 'element' (valid), with its descendants, at 'position' (DOM insertAdjacentElement). Throws
  // std::invalid_argument if 'element' is of another document, if the element where it would be inserted is 'element'
  // or one of its descendants, and for BeforeBegin and AfterEnd if this element is the root element or has no parent.
  void insertAdjacentElement(HtmlInsertPosition position, HtmlElement element);

  // Inserts a text node of 'text' (none if empty) at 'position' (DOM insertAdjacentText). Throws std::invalid_argument
  // for BeforeBegin and AfterEnd if the element is the root element or has no parent, std::bad_alloc on memory
  // exhaustion (nothing inserted).
  void insertAdjacentText(HtmlInsertPosition position, std::string_view text);

  // Moves 'child' after the last child of the element: insertAdjacentElement(HtmlInsertPosition::BeforeEnd, child).
  void appendChild(HtmlElement child) { insertAdjacentElement(HtmlInsertPosition::BeforeEnd, child); }

  // Takes the element, with its descendants, out of the tree (DOM remove()). It stays valid and can be inserted again.
  void remove() noexcept;

  // Two handles on the same element (or two invalid elements).
  bool operator==(const HtmlElement& other) const noexcept { return _pNode == other._pNode; }

 private:
  friend struct internal::HtmlNodeAccess;

  explicit HtmlElement(lxb_dom_node* pNode) noexcept : HtmlParentNode(pNode) {}
};

// The child elements of an element (forward range of HtmlElement). The child being visited can be removed or moved
// elsewhere: the iteration goes on with the element that followed it.
class HtmlChildren {
 public:
  class iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = HtmlElement;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = HtmlElement;

    iterator() noexcept = default;

    [[nodiscard]] HtmlElement operator*() const noexcept { return _element; }

    iterator& operator++() noexcept {
      *this = iterator(_next);
      return *this;
    }

    iterator operator++(int) noexcept {
      iterator ret = *this;
      ++*this;
      return ret;
    }

    // Two iterators on the same element.
    bool operator==(const iterator& other) const noexcept { return _element == other._element; }

   private:
    friend class HtmlChildren;

    explicit iterator(HtmlElement element) noexcept
        : _element(element), _next(element.valid() ? element.nextSibling() : HtmlElement()) {}

    HtmlElement _element;
    HtmlElement _next;  // read on arrival at _element, which can then be removed or moved (unlinked from the next one)
  };

  [[nodiscard]] iterator begin() const noexcept { return iterator(_first); }
  static iterator end() noexcept { return {}; }

  [[nodiscard]] bool empty() const noexcept { return !_first.valid(); }

 private:
  friend class HtmlElement;

  explicit HtmlChildren(HtmlElement first) noexcept : _first(first) {}

  HtmlElement _first;
};

inline HtmlChildren HtmlElement::children() const noexcept { return HtmlChildren(firstChild()); }

// Defined once HtmlElement is complete.
template <class Fn>
bool HtmlParentNode::CallElementCallback(HtmlElement element, void* pCallback) {
  Fn& callback = *static_cast<Fn*>(pCallback);
  if constexpr (std::is_same_v<std::remove_cvref_t<std::invoke_result_t<Fn&, HtmlElement>>, bool>) {
    return callback(element);
  } else {
    callback(element);
    return true;
  }
}

}  // namespace aeronet
