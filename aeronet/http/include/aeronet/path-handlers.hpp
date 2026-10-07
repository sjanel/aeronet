#pragma once

#include <functional>

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <memory>
#endif

#include "aeronet/http-response.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/request-task.hpp"
#endif

namespace aeronet {

class HttpRequestView;
class HttpResponseWriter;

// Classic request handler type: receives a const HttpRequestView& and returns an HttpResponse.
using RequestHandler = std::function<HttpResponse(const HttpRequestView&)>;

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
// Coroutine-friendly handler that may suspend while producing an HttpResponse.
using AsyncRequestHandler = std::function<RequestTask<HttpResponse>(HttpRequestView&)>;

// Storage of an async handler in the router: a request suspended in it keeps it alive (its coroutine may use the
// captures of the handler) even if the router replaces or removes it meanwhile.
using SharedAsyncRequestHandler = std::shared_ptr<AsyncRequestHandler>;

// Copy of the async handler held by 'handler' (nullptr if none), independent from it.
inline SharedAsyncRequestHandler CloneAsyncRequestHandler(const SharedAsyncRequestHandler& handler) {
  return handler ? std::make_shared<AsyncRequestHandler>(*handler) : nullptr;
}
#endif

// Streaming request handler type: receives a const HttpRequestView& and an HttpResponseWriter&
// Use it for large or long-lived responses where sending partial data before completion is beneficial.
using StreamingHandler = std::function<void(const HttpRequestView&, HttpResponseWriter&)>;

}  // namespace aeronet