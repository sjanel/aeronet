#pragma once

#include <quickjs.h>

namespace aeronet::internal {

class Page;

// Adds to 'natives', the object of the native helpers of the prelude (see PreludeSource()), those of its network and
// text APIs: loadNetwork() runs their part of the prelude (NetworkPreludeSource()); request(), requestSync() and
// cancelRequest() serve fetch(), XMLHttpRequest and navigator.sendBeacon(); decode() and encodeUtf8() serve
// TextDecoder, TextEncoder and the bodies; parseUrl() serves URL. Returns false on memory exhaustion, the exception
// pending.
[[nodiscard]] bool InstallNetwork(JSContext* pCtx, JSValueConst natives) noexcept;

// Sends the oldest request of the scripts of 'page' (see RequestQueue), then calls its callback with its response,
// or with its network error: a task of the page loop. Returns false, the exception pending, when the callback throws
// or on memory exhaustion of the JavaScript heap. Throws std::bad_alloc on that of the C++ code.
[[nodiscard]] bool RunNextRequest(Page& page);

}  // namespace aeronet::internal
