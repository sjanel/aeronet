#pragma once

#include <concepts>
#include <cstddef>
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

using HtmlElementCallback = void (*)(HtmlElement element, void* pCallback);
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

  // Calls callback(HtmlElement) for each descendant element matching selector, without allocating.
  // An exception thrown by callback stops the search and is propagated.
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
  static void CallElementCallback(HtmlElement element, void* pCallback);

  // The callback is given back to CallElementCallback with its own constness.
  template <class Fn>
  static void* ErasedPointer(Fn& callback) noexcept {
    return const_cast<void*>(static_cast<const void*>(std::addressof(callback)));
  }

  friend struct internal::HtmlNodeAccess;

  void forEachMatch(std::string_view selector, internal::HtmlElementCallback callback, void* pCallback) const;
  void forEachMatch(const CssSelector& selector, internal::HtmlElementCallback callback, void* pCallback) const;
};

// An attribute of an element. Its views point into the document, valid as long as it lives.
struct HtmlAttribute {
  std::string_view name;
  std::string_view value;
};

// The attributes of an element, in source order (forward range of HtmlAttribute).
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

    bool operator==(const iterator&) const noexcept = default;

   private:
    friend class HtmlAttributes;

    explicit iterator(lxb_dom_attr* pAttr) noexcept : _pAttr(pAttr) {}

    lxb_dom_attr* _pAttr{};
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

// An element of an HtmlDocument: a non-owning handle (one pointer, cheap to copy), valid as long as its document lives.
// A default-constructed element, or one returned when no element matches, is invalid: only valid() and comparisons can
// be called on it.
// Navigation goes through elements only (no text, comment or document node): parent(), children()...
class HtmlElement : public HtmlParentNode {
 public:
  // An invalid element.
  HtmlElement() noexcept = default;

  [[nodiscard]] bool valid() const noexcept { return _pNode != nullptr; }

  explicit operator bool() const noexcept { return valid(); }

  // The local name of the element, in lower case for HTML elements ("div", "tr"), unlike the upper-case tagName of the
  // DOM. The view points into the document.
  [[nodiscard]] std::string_view tagName() const noexcept;

  // The value of the attribute 'name' (matched in lower case for HTML elements), or an empty view if absent: see
  // hasAttribute() to tell an absent attribute from an empty one. The view points into the document.
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

  // The HTML serialization of the children of the element (DOM innerHTML).
  [[nodiscard]] std::string innerHtml() const;

  // The HTML serialization of the element and its children (DOM outerHTML).
  [[nodiscard]] std::string outerHtml() const;

  // Appends innerHtml(), or outerHtml(), to 'out'.
  void appendInnerHtml(std::string& out) const;
  void appendOuterHtml(std::string& out) const;

  // The parent element, invalid for the root element (<html>).
  [[nodiscard]] HtmlElement parent() const noexcept;

  // The first and last child elements, invalid if there are none.
  [[nodiscard]] HtmlElement firstChild() const noexcept;
  [[nodiscard]] HtmlElement lastChild() const noexcept;

  // The next and previous sibling elements, invalid if there are none.
  [[nodiscard]] HtmlElement nextSibling() const noexcept;
  [[nodiscard]] HtmlElement previousSibling() const noexcept;

  // The child elements, as a forward range: for (HtmlElement cell : row.children()) { ... }
  [[nodiscard]] HtmlChildren children() const noexcept;

  // Two handles on the same element (or two invalid elements).
  bool operator==(const HtmlElement& other) const noexcept { return _pNode == other._pNode; }

 private:
  friend struct internal::HtmlNodeAccess;

  explicit HtmlElement(lxb_dom_node* pNode) noexcept : HtmlParentNode(pNode) {}
};

// The child elements of an element (forward range of HtmlElement).
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
      _element = _element.nextSibling();
      return *this;
    }

    iterator operator++(int) noexcept {
      iterator ret = *this;
      ++*this;
      return ret;
    }

    bool operator==(const iterator&) const noexcept = default;

   private:
    friend class HtmlChildren;

    explicit iterator(HtmlElement element) noexcept : _element(element) {}

    HtmlElement _element;
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
void HtmlParentNode::CallElementCallback(HtmlElement element, void* pCallback) {
  (*static_cast<Fn*>(pCallback))(element);
}

}  // namespace aeronet
