#include <atomic>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/async-await-state.hpp"
#include "aeronet/async-handler-state.hpp"
#include "aeronet/async-resume-poster.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/cors-policy.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-request-dispatch.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/internal/http1-pipeline.hpp"
#include "aeronet/internal/pending-updates.hpp"
#include "aeronet/internal/server-context.hpp"
#include "aeronet/log.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/path-handlers.hpp"
#include "aeronet/protocol-handler.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/request-task.hpp"
#include "aeronet/router.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/http2-protocol-handler.hpp"
#endif

namespace aeronet {

void AsyncHandlerTask::start(RequestTask<HttpResponse> requestTask, SharedAsyncRequestHandler handler,
                             const CorsPolicy* pRouteCorsPolicy, std::span<const ResponseMiddleware> responseMiddleware,
                             AsyncResumePoster resumePoster) {
  assert(requestTask.valid());
  handlerKeepAlive = std::move(handler);
  coroutine = std::move(requestTask);
  pCorsPolicy = pRouteCorsPolicy;
  pResponseMiddleware = responseMiddleware.data();
  responseMiddlewareCount = static_cast<uint32_t>(responseMiddleware.size());
  await = AsyncAwaitState{.resumePoster = resumePoster};
  routeMayHaveChanged = false;
}

bool AsyncHandlerTask::run() {
  while (!coroutine.done()) {
    await.reason = AsyncAwaitState::Reason::None;
    coroutine.resume();
    if (await.reason != AsyncAwaitState::Reason::None) {
      // Suspended by an awaitable of the request: the event loop resumes it once its body or its deferred work is
      // ready.
      return false;
    }
    // Completed, or suspended by an awaitable that does not need the event loop: resumed right away.
  }
  return true;
}

HttpResponse AsyncHandlerTask::takeResponse(const HttpRequestView& request) {
  assert(coroutine.done());
  HttpResponse response = internal::HttpResponseAccess::MakeUnallocated();
  try {
    // Already completed: returns its result, or rethrows its exception.
    response = coroutine.runSynchronously();
  } catch (const std::exception& ex) {
    log::error("Exception in async path handler for path {}: {}", request.path(), ex.what());
    response = request.makeResponse(http::StatusCodeInternalServerError);
  } catch (...) {
    log::error("Unknown exception in async path handler for path {}", request.path());
    response = request.makeResponse(http::StatusCodeInternalServerError);
  }
  reset();
  return response;
}

const CorsPolicy* AsyncHandlerTask::applyResponseMiddleware(const RequestPipeline& pipeline,
                                                            const HttpRequestView& request,
                                                            HttpResponse& response) const {
  std::span<const ResponseMiddleware> middleware(pResponseMiddleware, responseMiddlewareCount);
  const CorsPolicy* pRouteCorsPolicy = pCorsPolicy;
  if (routeMayHaveChanged) {
    const Router::RoutingResult routingResult = pipeline.router().match(request.method(), request.path());
    middleware = routingResult.postMiddlewareRange();
    pRouteCorsPolicy = routingResult.corsPolicy();
  }
  pipeline.applyResponseMiddleware(request, response, middleware, false);
  return pRouteCorsPolicy;
}

namespace internal {

bool Http1Pipeline::PauseReadingDuringAsyncHandler(ConnectionState& state) noexcept {
  const AsyncHandlerState* asyncState = state.pAsyncState();
  if (asyncState == nullptr || !asyncState->holdsInput()) {
    return false;
  }
  // The input buffer holds the body (and the bytes) of the request of the async handler: it must neither grow nor move
  // until it completes. The next bytes are left in the socket (which also bounds them) and read once it completed,
  // see SingleHttpServer::resumeInputAfterAsyncHandler().
  state.readPaused = true;
  return true;
}

bool Http1Pipeline::dispatchAsyncHandler(ConnectionIt cnxIt, const SharedAsyncRequestHandler& handler, bool bodyReady,
                                         bool isChunked, bool found100Continue, std::size_t consumedBytes,
                                         const CorsPolicy* pCorsPolicy,
                                         std::span<const ResponseMiddleware> responseMiddleware,
                                         std::size_t perRouteMaxBodyBytes) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;
  RequestTask<HttpResponse> requestTask = RequestPipeline::CallAsyncHandler(*handler, request);

  if (!requestTask.valid()) {
    HttpResponse resp(http::StatusCodeInternalServerError, "Async handler inactive");
    ctx.requestPipeline().applyResponseMiddleware(request, resp, responseMiddleware, false);
    // Answered before its body is received: the connection closes, the rest of the body not being read.
    finalizeAndSendResponse(cnxIt, std::move(resp), pCorsPolicy, !bodyReady);
    return false;
  }

  auto& asyncState = state.ensureAsyncState(ctx.connections.asyncHandlerStatePool());

  asyncState.task.start(std::move(requestTask), handler, pCorsPolicy, responseMiddleware,
                        ctx.asyncResumePoster(cnxIt->fd(), state.generation));
  asyncState.task.await.needsBody = !bodyReady;
  asyncState.bodyReceived = bodyReady;
  asyncState.active = true;
  asyncState.isChunked = isChunked;
  asyncState.maxBodyBytes = perRouteMaxBodyBytes;
  asyncState.pendingResponse = internal::HttpResponseAccess::MakeUnallocated();
  state.pendingRequest.continueExpected = found100Continue && !bodyReady;

  // Keep header storage stable while async work runs so header string_views stay valid
  request.pinHeadStorage(asyncState.headBuffer);
  asyncState.task.bind(request);

  resumeAsyncHandler(cnxIt);
  if (asyncState.active) {
    // Completes later, erasing the request then. The router may be updated meanwhile.
    asyncState.consumedBytes = bodyReady ? consumedBytes : 0;
    asyncState.task.routeMayHaveChanged = true;
  }

  return true;
}

void Http1Pipeline::resumeAsyncHandler(ConnectionIt cnxIt) {
  ConnectionState& state = _pCtx->connections.connectionState(cnxIt);
  auto* asyncState = state.pAsyncState();
  assert(asyncState != nullptr);
  auto& async = *asyncState;
  if (!async.active || !async.task.coroutine.valid()) {
    return;
  }

  if (async.bodyReceived && async.task.await.needsBody && !finishAsyncBody(cnxIt, async)) {
    return;
  }

  if (async.task.run()) {
    onAsyncHandlerCompleted(cnxIt);
  } else if (state.pendingRequest.continueExpected &&
             async.task.await.reason == AsyncAwaitState::Reason::WaitingForBody) {
    state.pendingRequest.continueExpected = false;
    queue100Continue(cnxIt);
  } else if (RawChars& sharedBody = _pCtx->sharedBuffers.decompressedBody;
             state.request._body.data() == sharedBody.data() && !sharedBody.empty()) {
    // Suspended with its decompressed body at the start of the shared buffer, followed by its updated Content-Length
    // (hence not empty, even for an empty body), that the next compressed request of any connection would overwrite.
    // Its connection takes the buffer over: the bytes do not move, so the views of the handler stay valid, even if its
    // deferred work is already reading them. The shared buffer gets the previous capacity of the connection, grown
    // again by the next compressed request: as fast as copying the body out (DeferredGzipPost in
    // http1-pipeline_bench.cpp), and a suspended handler holds its body either way. reclaimMemoryFromOversizedBuffers()
    // shrinks it once released.
    assert(state.pendingRequest.trailerLen == 0);  // the buffer of the connection holds nothing of this request
    sharedBody.swap(state.bodyAndTrailersBuffer);
  }

  if (async.active) {
    // It may have just started waiting for its body, or stopped: the timers of its client follow. They restart now, the
    // client not being responsible for the time spent by the deferred work.
    state.lastActivity = std::chrono::steady_clock::now();
    _pCtx->maintenance.refreshKeepAliveDeadline(cnxIt);
    _pCtx->maintenance.trackBodyReception(state, async.isWorking());
  }
}

bool Http1Pipeline::finishAsyncBody(ConnectionIt cnxIt, AsyncHandlerState& async) {
  // The request is modified below: never while deferred work may read it.
  assert(!async.task.isAwaitingCallback());
  ConnectionState& state = _pCtx->connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;
  if (!async.isChunked) {
    // The request starts the input buffer, where its body is received.
    const std::size_t headSize = request.headSpanSize();
    request._body = std::string_view(state.inBuffer.data() + headSize, async.consumedBytes - headSize);
  }
  const bool finished = async.isChunked ? finishDeferredChunkedBody(cnxIt) : maybeDecompressRequestBody(cnxIt, true);
  if (!finished) {
    AbandonAsyncHandler(async);
    return false;
  }
  state.installAggregatedBodyBridge();
  async.task.await.needsBody = false;
  return true;
}

void Http1Pipeline::AbandonAsyncHandler(AsyncHandlerState& async) noexcept {
  if (async.task.isAwaitingCallback()) {
    // The deferred work may still use the coroutine frame and the request: they are released when it completes.
    async.active = false;
    async.pendingResponse = internal::HttpResponseAccess::MakeUnallocated();
  } else {
    async.clear();
  }
}

void Http1Pipeline::handleAsyncBodyProgress(ConnectionIt cnxIt) {
  ConnectionState& state = _pCtx->connections.connectionState(cnxIt);
  AsyncHandlerState* asyncState = state.pAsyncState();
  assert(asyncState != nullptr);
  AsyncHandlerState& async = *asyncState;
  assert(async.active);

  if (!async.bodyReceived) {
    std::size_t consumedBytes = 0;
    // The request starts the input buffer, where its body is received.
    static constexpr bool kDeferBody = true;
    const BodyDecodeStatus status =
        decodeBodyIfReady(cnxIt, state.inBuffer, async.isChunked, async.maxBodyBytes, consumedBytes, kDeferBody);
    if (status == BodyDecodeStatus::Error) {
      AbandonAsyncHandler(async);
      return;
    }
    if (status == BodyDecodeStatus::NeedMore) {
      return;
    }

    async.bodyReceived = true;
    async.consumedBytes = consumedBytes;
    _pCtx->maintenance.trackBodyReception(state, true);

    // resumeAsyncHandler() makes the body readable: not while deferred work may read the request, and not at all once
    // the handler answered.
    if (async.task.await.reason == AsyncAwaitState::Reason::WaitingForBody) {
      resumeAsyncHandler(cnxIt);
      return;
    }
  }

  if (!async.pendingResponse.empty()) {
    tryFlushPendingAsyncResponse(cnxIt);
  }
}

void Http1Pipeline::onAsyncHandlerCompleted(ConnectionIt cnxIt) {
  ConnectionState& state = _pCtx->connections.connectionState(cnxIt);
  AsyncHandlerState& async = *state.pAsyncState();
  async.pendingResponse = async.task.takeResponse(state.request);
  if (async.bodyReceived) {
    tryFlushPendingAsyncResponse(cnxIt);
  } else if (state.pendingRequest.continueExpected) {
    // Its client may wait for 100 Continue, not sent as the handler did not wait for the body: it is answered right
    // away, the connection closing since its body may still come (RFC 9110 §10.1.1).
    tryFlushPendingAsyncResponse(cnxIt, true);
  }
  // Otherwise, the request is answered once the rest of its body is received (and drained).
}

void Http1Pipeline::tryFlushPendingAsyncResponse(ConnectionIt cnxIt, bool close) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  AsyncHandlerState& async = *state.pAsyncState();

  assert(close || async.bodyReceived);
  assert(!async.pendingResponse.empty());
  // Answered: its response middleware must not expect a body that will never be readable.
  async.task.await.needsBody = false;

  const CorsPolicy* pCorsPolicy =
      async.task.applyResponseMiddleware(ctx.requestPipeline(), state.request, async.pendingResponse);
  finalizeAndSendResponse(cnxIt, std::move(async.pendingResponse), pCorsPolicy, close);
  state.inBuffer.erase_front(async.consumedBytes);
  async.clear();
  state.lastActivity = std::chrono::steady_clock::now();
  ctx.maintenance.refreshKeepAliveDeadline(cnxIt);
}

}  // namespace internal

void SingleHttpServer::resumeInputAfterAsyncHandler(NativeHandle fd) {
  internal::ConnectionStorage& connections = _ctx.connections;
  auto cnxIt = connections.iterator(fd);
  ConnectionState* pState = connections.pConnectionState(cnxIt);
  // Serve the requests pipelined behind the completed one: they were already read, no event will report them.
  if (!pState->inBuffer.empty()) {
    (void)processHttp1Input(cnxIt);
    cnxIt = connections.iterator(fd);
    pState = connections.pConnectionState(cnxIt);
  }
  // Then read the bytes left in the socket while reading was paused: edge-triggered polling does not report them again.
  if (pState->readPaused && !pState->isAnyCloseRequested()) {
    pState->readPaused = false;
    if (handleReadableClient(cnxIt, false) == CloseStatus::Close) {
      const auto finalIt = connections.iterator(fd);
      if (IsValid(connections, finalIt)) {
        _ctx.io.closeConnection(finalIt);
      }
    }
  } else {
    // The input buffer held the request of the handler, read into the input buffer of the event loop.
    _ctx.io.releaseServedInBuffer(*pState);
  }
}

void SingleHttpServer::processAsyncCallbacks() {
  internal::ConnectionStorage& connections = _ctx.connections;
  vector<internal::PendingUpdates::AsyncCallback> callbacks;
  {
    std::scoped_lock lock(_ctx.updates.lock);
    callbacks.swap(_ctx.updates.asyncCallbacks);
    _ctx.updates.hasAsyncCallbacks.store(false, std::memory_order_release);
  }

  for (auto& cb : callbacks) {
    try {
      auto it = connections.findConnection(cb.connectionFd, cb.connectionGeneration);
      if (it == connections.end()) {
        // The connection was closed while the work was running: its coroutine was kept for it, release it now.
        connections.releaseOrphanedAsyncTask(cb.connectionGeneration, cb.handle, _ctx.config.maxCachedConnections);
        continue;
      }

      ConnectionState& state = connections.connectionState(it);
#ifdef AERONET_ENABLE_HTTP2
      // For HTTP/2 connections, delegate to the protocol handler which tracks per-stream async state.
      if (state.protocol == ProtocolType::Http2 && state.protocolHandler != nullptr) {
        auto* pH2Handler = static_cast<http2::Http2ProtocolHandler*>(state.protocolHandler.get());
        if (pH2Handler->resumeAsyncTaskByHandle(cb.handle)) {
          // Flush any pending output generated by the completed async handler
          if (pH2Handler->hasPendingOutput()) {
            _ctx.io.flushOutbound(it);
          }
          // Deferred work that outlived keepAliveTimeout leaves a stale idle deadline behind, exactly as
          // a slow synchronous handler does (HTTP/1 goes through tryFlushPendingAsyncResponse instead).
          _ctx.maintenance.restartKeepAliveIdleWindow(cb.connectionFd);
        }
      } else
#endif
      {
        // The coroutine frame is kept until its deferred work completes: a coroutine created meanwhile cannot have
        // the same address, the handle identifies it.
        auto* asyncState = state.pAsyncState();
        if (asyncState != nullptr && asyncState->task.runs(cb.handle)) {
          asyncState->task.onWorkCompleted();
          if (asyncState->active) {
            _ctx.http1.resumeAsyncHandler(it);
            if (!asyncState->active) {
              resumeInputAfterAsyncHandler(cb.connectionFd);
            }
          } else {
            // Abandoned (its connection is closing) while the work was running.
            asyncState->clear();
          }
        }
      }
      // Close connection immediately if response was sent and drain-and-close is pending.
      // On platforms without a real timer fd (Windows, macOS), sweep maintenance may not
      // run often enough, causing Connection: close requests to linger.
      it = connections.findConnection(cb.connectionFd, cb.connectionGeneration);
      if (it != connections.end() && connections.connectionState(it).canCloseConnectionForDrain()) {
        _ctx.io.closeConnection(it);
      }
    } catch (const std::exception& ex) {
      log::error("Exception processing async callback for fd # {}: {}", static_cast<uintptr_t>(cb.connectionFd),
                 ex.what());
    } catch (...) {
      log::error("Unknown exception processing async callback for fd # {}", static_cast<uintptr_t>(cb.connectionFd));
    }
  }
}

void SingleHttpServer::waitForOrphanedAsyncWork() {
  if (!_ctx.lifecycle.isIdle()) {
    // Still run by another thread, which owns the connections.
    return;
  }
  // Deferred work still running uses the coroutine frame and the request kept for it, and completes by posting to this
  // server: wait for it.
  static constexpr auto kLogPeriod = std::chrono::seconds{5};
  auto nextLog = std::chrono::steady_clock::now() + kLogPeriod;
  while (_ctx.connections.hasOrphanedConnectionStates()) {
    if (_ctx.updates.hasAsyncCallbacks.load(std::memory_order_acquire)) {
      processAsyncCallbacks();
      continue;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextLog) {
      log::warn("Waiting for the deferred work still running for {} closed connection(s)",
                _ctx.connections.nbOrphanedConnectionStates());
      nextLog = now + kLogPeriod;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

}  // namespace aeronet
