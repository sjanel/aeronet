#pragma once

#include <string_view>
#include <utility>

struct lxb_css_selector_list;

namespace aeronet {

namespace internal {
struct HtmlNodeAccess;
}  // namespace internal

// A compiled CSS selector list (Selectors Level 4: type, class, id and attribute selectors, combinators, :not(), :is(),
// :where(), :has(), :nth-child()...), matched against the elements of an HtmlDocument without being parsed again.
// Compile a selector used many times (in a loop, or on many documents) once, and pass it to querySelector(),
// querySelectorAll() or forEach() instead of its text.
class CssSelector {
 public:
  // An empty selector: a placeholder that a compiled one can be assigned to, it cannot be passed to the queries.
  CssSelector() noexcept = default;

  // Compiles 'selector', e.g. "table.fees tr > td:first-child, a[href]".
  // Throws std::invalid_argument if it is not a valid selector list, std::bad_alloc on memory exhaustion.
  explicit CssSelector(std::string_view selector);

  CssSelector(const CssSelector&) = delete;
  CssSelector& operator=(const CssSelector&) = delete;

  // The moved-from selector is empty.
  CssSelector(CssSelector&& other) noexcept : _pList(std::exchange(other._pList, nullptr)) {}
  CssSelector& operator=(CssSelector&& other) noexcept;

  ~CssSelector();

  // Whether this selector is empty: default constructed or moved from.
  [[nodiscard]] bool empty() const noexcept { return _pList == nullptr; }

 private:
  friend struct internal::HtmlNodeAccess;

  lxb_css_selector_list* _pList{};
};

}  // namespace aeronet
