#pragma once

#include <quickjs.h>

#include <string_view>

#include "aeronet/raw-bytes.hpp"
#include "js-engine.hpp"

namespace aeronet::internal {

// The JavaScript part of the Web APIs of a page, run in each page after the native bindings: EventTarget, Event,
// CustomEvent, classList, dataset, style, console, history, requestAnimationFrame, the ParentNode / ChildNode helpers,
// and the globals of NetworkPreludeSource(), set up on their first use. It is a function expression: called with the
// object of the native helpers it needs (log, reportError, setUrl, resolveUrl, and those of InstallNetwork()), it
// returns the object of the functions the page calls (dispatch). Null-terminated at size().
std::string_view PreludeSource() noexcept;

// The second part, run by the native loadNetwork() of InstallNetwork(): fetch(), Headers, Request, Response,
// AbortController, AbortSignal, XMLHttpRequest, ProgressEvent, navigator.sendBeacon(), TextEncoder, TextDecoder, URL,
// URLSearchParams. A function expression: called with the native helpers and those of the first part ({getters,
// Event, EventTarget}), it returns the object of its globals. Null-terminated at size().
std::string_view NetworkPreludeSource() noexcept;

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
