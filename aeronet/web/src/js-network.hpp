#pragma once

#include <quickjs.h>

#include <expected>
#include <string_view>

#include "aeronet/http-client.hpp"
#include "aeronet/http-response.hpp"
#include "cors.hpp"
#include "request-queue.hpp"

namespace aeronet::internal {

class Page;

// Adds to 'natives', the object of the native helpers of the prelude (see PreludeSource()), those of its network and
// text APIs (LazyPrelude::Network): request(), requestSync() and cancelRequest() serve fetch(), XMLHttpRequest and
// navigator.sendBeacon(); decode(), decodeUtf8() and encodeUtf8() serve TextDecoder, TextEncoder and the bodies;
// parseUrl() serves URL. Returns false on memory exhaustion, the
// exception pending.
[[nodiscard]] bool InstallNetwork(JSContext* pCtx, JSValueConst natives) noexcept;

// Sends the oldest request of the scripts of 'page' (see RequestQueue), then calls its callback with its response,
// or with its network error: a task of the page loop. Returns false, the exception pending, when the callback throws
// or on memory exhaustion of the JavaScript heap. Throws std::bad_alloc on that of the C++ code.
[[nodiscard]] bool RunNextRequest(Page& page);

// The final response of a request of 'page' (see Fetch()), or the reason of its network error.
using FetchResult = std::expected<HttpResponse, const char*>;

// The GET of a script of 'page' at 'url' (absolute, http or https, without fragment) in 'mode' with 'credentials', with
// its Accept, its Origin, and the Referer of 'referrerUrl' (an http or https URL without user information: the
// document, or the importing module): the request of a module. Throws std::bad_alloc on memory exhaustion.
[[nodiscard]] RequestQueue::Request ScriptRequest(Page& page, std::string_view url, RequestMode mode,
                                                  RequestCredentials credentials, std::string_view referrerUrl);

// Sends 'request' of 'page' with the rules of the Fetch Standard: the CORS preflights it needs, at its URL and after a
// redirect to another origin, the rules of each redirect (see PageRequestContext::onRedirect), and the CORS check of
// the final response of a request to another origin in the cors mode. request.http ends at the URL of the final
// response, and request.crossOrigin and request.redirected tell what the redirects did. Throws std::bad_alloc on
// memory exhaustion.
[[nodiscard]] FetchResult Fetch(Page& page, RequestQueue::Request& request);

}  // namespace aeronet::internal
