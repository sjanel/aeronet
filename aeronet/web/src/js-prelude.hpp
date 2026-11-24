#pragma once

#include <quickjs.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/raw-bytes.hpp"
#include "js-engine.hpp"

namespace aeronet::internal {

// The JavaScript part of the Web APIs of a page, run in each page after the native bindings: EventTarget, Event,
// CustomEvent, classList, dataset, style, console, history, requestAnimationFrame, the ParentNode / ChildNode helpers,
// and the layout stubs of the elements. It is a function expression: called with the object of the native helpers it
// needs (log, reportError, setUrl, resolveUrl, replaceAll, those of InstallNetwork(), and the object of the lazy
// globals, which it inserts between Window.prototype and EventTarget.prototype: see InstallWindow()), it returns the
// object of the functions the page calls (dispatch) and the helpers of the lazy parts (shared). Null-terminated at
// size().
std::string_view PreludeSource() noexcept;

// The parts of the prelude set up in a page by Page::lazyPart() on the first read of one of their globals (see
// InstallWindow()) or the first call of a method that uses them (Number.prototype.toLocaleString()...): most pages use
// none of them, and do not pay for them. Each is a function expression: called with the native helpers and 'shared',
// helpers of the first part ({getters, Event, EventTarget, CSSStyleDeclaration}), it returns the object of its globals.
enum class LazyPrelude : uint8_t {
  // fetch(), Headers, Request, Response, AbortController, AbortSignal, XMLHttpRequest, ProgressEvent,
  // navigator.sendBeacon(), TextEncoder, TextDecoder, URL, URLSearchParams.
  Network,
  // DOMRect, DOMRectReadOnly, IntersectionObserver, ResizeObserver, MutationObserver, MediaQueryList, matchMedia(),
  // getComputedStyle(): no layout, every element is visible and empty, no mutation reported; NodeList, HTMLCollection.
  View,
  // Intl, which QuickJS-ng does not have: en-US formatting.
  Intl,
};

inline constexpr std::size_t kNbLazyPreludes = 3;

// The source of the lazy part 'part', null-terminated at size().
std::string_view LazyPreludeSource(LazyPrelude part) noexcept;

// The file name of the lazy part 'part' in the stack traces ("aeronet:network").
const char* LazyPreludeName(LazyPrelude part) noexcept;

// The function of the prelude part 'source' (named 'filename' in the stack traces), read from its bytecode
// 'bytecode', which the first page of a session that runs it compiles for the next ones. The bytecode has no source
// nor debug information: smaller and faster to read, and its functions show "[native code]", as the built-ins of
// browsers. The compilation ignores the 'limits' of the page, code of aeronet of a bounded size and depth, which the
// parser of QuickJS-ng does not survive near them: it reports a stack overflow in its lookahead as a bogus syntax
// error (js_parse_skip_parens_token drops the failure of next_token()), and writes before the label array of a
// function when an allocation fails in a destructuring pattern (js_parse_destructuring_element uses the -1 of a failed
// new_label()). Returns the function, owned, or JS_EXCEPTION with the exception pending.
[[nodiscard]] JSValue PreludeFunction(JSContext* pCtx, std::string_view source, const char* filename,
                                      RawBytes& bytecode, const JsLimits& limits);

}  // namespace aeronet::internal
