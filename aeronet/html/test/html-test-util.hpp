#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <iterator>
#include <new>
#include <string>
#include <string_view>

#include "aeronet/format-lite.hpp"
#include "aeronet/html-document.hpp"
#include "aeronet/html-element.hpp"
#include "aeronet/sys-test-support.hpp"

namespace aeronet::test {

inline std::string Repeat(std::string_view text, std::size_t count) {
  std::string ret;
  ret.reserve(text.size() * count);
  for (std::size_t idx = 0; idx < count; ++idx) {
    ret.append(text);
  }
  return ret;
}

// The number of elements from the top of the tree of 'element' to 'element', both included.
inline std::size_t Depth(HtmlElement element) {
  std::size_t depth = 0;
  for (; element; element = element.parent()) {
    ++depth;
  }
  return depth;
}

inline std::size_t ChildCount(HtmlElement element) {
  return static_cast<std::size_t>(std::ranges::distance(element.children()));
}

inline std::size_t AttributeCount(HtmlElement element) {
  return static_cast<std::size_t>(std::ranges::distance(element.attributes()));
}

// The attributes ' <prefix>0=0 <prefix>1=1 ...' of a start tag, 'count' of them.
inline std::string Attributes(std::string_view prefix, std::size_t count) {
  std::string ret;
  for (std::size_t idx = 0; idx < count; ++idx) {
    FormatTo(ret, " {}{}={}", prefix, idx, idx);
  }
  return ret;
}

#if AERONET_WANT_MALLOC_OVERRIDES
// Fails each allocation of 'operation(HtmlDocument&)' in turn, until it needs no more, each time on a new document
// parsed from 'html' without failure (lexbor crashes on some failed allocations of the parsing, see
// html-document_test.cpp). After each std::bad_alloc, check(const HtmlDocument&) checks that the document is still
// usable.
template <class Operation, class Check>
void ExpectBadAllocOnEachAllocation(std::string_view html, Operation operation, Check check) {
  for (int successfulAllocs = 0;; ++successfulAllocs) {
    HtmlDocument doc(html);
    FailNextMalloc(successfulAllocs, 1);
    try {
      operation(doc);
      ResetToSysMalloc();
      return;
    } catch (const std::bad_alloc&) {
      ResetToSysMalloc();
    }
    check(static_cast<const HtmlDocument&>(doc));
    ASSERT_LT(successfulAllocs, 10000);
  }
}

template <class Operation>
void ExpectBadAllocOnEachAllocation(std::string_view html, Operation operation) {
  ExpectBadAllocOnEachAllocation(html, operation, [](const HtmlDocument&) {});
}
#endif

}  // namespace aeronet::test
