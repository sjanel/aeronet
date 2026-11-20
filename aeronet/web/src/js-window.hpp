#pragma once

#include <quickjs.h>

namespace aeronet::internal {

class Page;

// Defines the window of a page on its global object: window, self, frames, parent, top, name, opener, closed, origin,
// location (and its accessor, so that assigning it navigates), navigator, localStorage, sessionStorage, and the
// timers (setTimeout(), setInterval(), clearTimeout(), clearInterval()). Returns the object of the native helpers
// that the prelude takes (see PreludeSource()), owned, or JS_EXCEPTION on memory or stack exhaustion, the exception
// pending. Throws std::bad_alloc on memory exhaustion of the C++ code.
[[nodiscard]] JSValue InstallWindow(Page& page);

}  // namespace aeronet::internal
