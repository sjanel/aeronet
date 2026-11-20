#pragma once

#include <string_view>

namespace aeronet::internal {

// The JavaScript part of the Web APIs of a page, run in each page after the native bindings: EventTarget, Event,
// CustomEvent, classList, dataset, style, console, history, requestAnimationFrame and the ParentNode / ChildNode
// helpers. It is a function expression: called with the object of the native helpers it needs (log, reportError,
// setUrl, resolveUrl), it returns the object of the functions the page calls (dispatch). Null-terminated at size().
std::string_view PreludeSource() noexcept;

}  // namespace aeronet::internal
