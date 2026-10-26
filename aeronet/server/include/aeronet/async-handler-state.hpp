#pragma once

#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

#include "aeronet/async-await-state.hpp"
#include "aeronet/async-resume-poster.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/internal/http-response-access.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/object-pool.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/request-task.hpp"

namespace aeronet {

class CorsPolicy;
class RequestPipeline;

// The coroutine of an async handler and what its completion needs, shared by the HTTP/1.1 connections (see
// AsyncHandlerState) and the HTTP/2 streams: the handler, kept alive while the coroutine may use it, the state that the
// awaitables of its request update (see AsyncAwaitState), and the response middleware and CORS policy of its route.
struct AsyncHandlerTask {
  // Takes the coroutine of an async handler just called (not started yet), whose route has the given CORS policy and
  // response middleware. Its deferred work posts its resumption with resumePoster.
  void start(RequestTask<HttpResponse> requestTask, SharedAsyncRequestHandler handler,
             const CorsPolicy* pRouteCorsPolicy, std::span<const ResponseMiddleware> responseMiddleware,
             AsyncResumePoster resumePoster);

  // Points the request to the await state of this task, which must stay at the same address while the coroutine runs.
  void bind(HttpRequestView& request) noexcept { request._pAsyncAwait = &await; }

  // Resumes the coroutine until it completes (returns true) or suspends to wait for the event loop (returns false): for
  // the rest of the body of its request, or for the completion of deferred work. A suspension by another awaitable
  // (std::suspend_always for instance) does not need the event loop: the coroutine is resumed right away.
  bool run();

  // Tells whether the coroutine waits for the completion of deferred work, still running in the background: the work
  // may use the coroutine frame and the request until then.
  [[nodiscard]] bool isAwaitingCallback() const noexcept {
    return coroutine.valid() && await.reason == AsyncAwaitState::Reason::WaitingForCallback;
  }

  // Tells whether this task runs the coroutine of 'handle'.
  [[nodiscard]] bool runs(std::coroutine_handle<> handle) const noexcept {
    return coroutine.valid() && coroutine.coroutineAddress() == handle.address();
  }

  // Takes the response of the completed coroutine (a 500 when it threw, logged), destroying it and releasing the
  // handler.
  [[nodiscard]] HttpResponse takeResponse(const HttpRequestView& request);

  // Runs the response middleware of the route of the request on its response, and returns the CORS policy of the route,
  // to apply when finalizing it. The route is looked up again if the router may have been updated since the dispatch
  // of the handler (routeMayHaveChanged), possibly destroying the metadata of the route.
  const CorsPolicy* applyResponseMiddleware(const RequestPipeline& pipeline, const HttpRequestView& request,
                                            HttpResponse& response) const;

  // Destroys the coroutine first (it may use the handler), then releases the handler.
  void reset() noexcept {
    coroutine.reset();
    handlerKeepAlive.reset();
  }

  // Keeps the handler (whose captures the coroutine may use) alive until the coroutine is destroyed, even if the router
  // is updated meanwhile. Declared before the coroutine so that it is destroyed after it.
  std::shared_ptr<const void> handlerKeepAlive;
  RequestTask<HttpResponse> coroutine;
  const CorsPolicy* pCorsPolicy{nullptr};
  const ResponseMiddleware* pResponseMiddleware{nullptr};
  // The next members fill its tail padding.
  [[no_unique_address]] AsyncAwaitState await;
  // True once the handler outlived its dispatch (suspended, or waiting for its body): the router may have been updated
  // since, and the CORS policy and response middleware of its route must then be looked up again.
  bool routeMayHaveChanged{false};
  uint32_t responseMiddlewareCount{0};
};

// The async handler of the current request of an HTTP/1.1 connection.
struct AsyncHandlerState {
  // Tells whether the connection input must be left untouched: the whole request of the running handler was received
  // (the input buffer holds its body), or the request was abandoned while deferred work may still use it.
  [[nodiscard]] bool holdsInput() const noexcept { return active ? !task.await.needsBody : task.coroutine.valid(); }

  // Resets this state, destroying the coroutine frame first: it may still use the handler.
  void clear() noexcept {
    task.reset();
    *this = {};
  }

  // stable storage for the current request head when async body progress is needed
  RawChars headBuffer;
  AsyncHandlerTask task;
  std::size_t consumedBytes{0};
  // Per-route maximum body size override (MAX = use global limit only).
  std::size_t maxBodyBytes = static_cast<std::size_t>(-1);
  HttpResponse pendingResponse = internal::HttpResponseAccess::MakeUnallocated();
  bool active{false};
  bool usesSharedDecompressedBody{false};
  bool isChunked{false};
};

using AsyncHandlerStatePool = ObjectPool<AsyncHandlerState>;

}  // namespace aeronet
