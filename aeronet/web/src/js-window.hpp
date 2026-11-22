#pragma once

#include <quickjs.h>

namespace aeronet::internal {

class Page;

// Defines the window of a page on its global object: window, self, frames, parent, top, name, opener, closed, origin,
// location (and its accessor, so that assigning it navigates), navigator, localStorage, sessionStorage, the timers
// (setTimeout(), setInterval(), clearTimeout(), clearInterval()), the viewport and the screen of a desktop browser
// without layout, and the methods of the built-ins that format with the Intl part of the prelude. Creates the object of
// the lazy globals (Page::lazyGlobals()), whose own properties are the globals of the lazy parts of the prelude and the
// HTML element interfaces, set up on their first read: an assignment before replaces one without setting it up.
// Returns the object of the native helpers that the prelude takes (see PreludeSource()), owned, or JS_EXCEPTION on
// memory or stack exhaustion, the exception pending. Throws std::bad_alloc on memory exhaustion of the C++ code.
[[nodiscard]] JSValue InstallWindow(Page& page);

}  // namespace aeronet::internal
