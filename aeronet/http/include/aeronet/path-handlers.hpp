#pragma once

#include <functional>

#include "aeronet/http-response.hpp"
#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include "aeronet/request-task.hpp"
#endif

namespace aeronet {

class HttpRequest;
class HttpResponseWriter;

// Classic request handler type: receives a const HttpRequest& and returns an HttpResponse.
using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
// Coroutine-friendly handler that may suspend while producing an HttpResponse.
using AsyncRequestHandler = std::function<RequestTask<HttpResponse>(HttpRequest&)>;
#endif

// Streaming request handler type: receives a const HttpRequest& and an HttpResponseWriter&
// Use it for large or long-lived responses where sending partial data before completion is beneficial.
using StreamingHandler = std::function<void(const HttpRequest&, HttpResponseWriter&)>;

}  // namespace aeronet