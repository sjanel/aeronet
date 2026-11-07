#pragma once

#include <cstddef>
#include <string_view>

struct lxb_html_document;

namespace aeronet::internal {

// The tree construction of HTML walks the stack of open elements for most start tags (the "has an element in scope"
// checks), and recreates the list of active formatting elements that are no longer open before most text and start
// tags (misnested <b>, <font>...): unbounded, both make the parsing quadratic, in time for deeply nested elements, in
// time and nodes for a list that keeps growing (150 KB of "<p><b id=N>x</p>" created 32 million elements).

// The maximum number of open elements, <html> included: the depth limit of browsers. A start tag at this depth first
// closes the current element, as if its end tag preceded it: deeper elements become siblings instead of children.
inline constexpr std::size_t kMaxOpenElements = 512;

// The maximum number of entries of the list of active formatting elements after its last marker (that is, in the
// current table cell, template...). Beyond it, the earliest entries are removed, as the Noah's Ark clause of HTML
// removes the earliest of 4 identical ones: they are no longer recreated.
inline constexpr std::size_t kMaxActiveFormattingElements = 16;

// Parses 'html' (UTF-8) into pDocument, a new document, with the limits above.
// Throws std::bad_alloc on memory exhaustion.
void ParseHtml(lxb_html_document* pDocument, std::string_view html);

}  // namespace aeronet::internal
