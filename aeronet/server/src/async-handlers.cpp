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
  const auto* asyncState = state.pAsyncState();
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
                                         bool isChunked, std::size_t consumedBytes, const CorsPolicy* pCorsPolicy,
                                         std::span<const ResponseMiddleware> responseMiddleware,
                                         std::size_t perRouteMaxBodyBytes) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;
  RequestTask<HttpResponse> requestTask = RequestPipeline::CallAsyncHandler(*handler, request);

  if (!requestTask.valid()) {
    static constexpr std::string_view kMessage = "Async handler inactive";
    if (bodyReady) {
      HttpResponse resp(http::StatusCodeInternalServerError, kMessage);
      ctx.requestPipeline().applyResponseMiddleware(request, resp, responseMiddleware, false);
      finalizeAndSendResponse(cnxIt, std::move(resp), pCorsPolicy);
    } else {
      emitSimpleError(cnxIt, http::StatusCodeInternalServerError, kMessage);
    }

    return false;
  }

  auto& asyncState = state.ensureAsyncState(ctx.connections.asyncHandlerStatePool());
  const std::string_view bodyView = state.request._body;
  bool usesSharedDecompressedBody = false;
  if (bodyReady && !bodyView.empty()) {
    const char* sharedBeg = ctx.sharedBuffers.decompressedBody.data();
    const char* sharedEnd = sharedBeg + ctx.sharedBuffers.decompressedBody.size();
    const char* bodyBeg = bodyView.data();
    const char* bodyEnd = bodyBeg + bodyView.size();

    usesSharedDecompressedBody = sharedBeg <= bodyBeg && bodyEnd <= sharedEnd;
  }

  asyncState.task.start(std::move(requestTask), handler, pCorsPolicy, responseMiddleware,
                        ctx.asyncResumePoster(cnxIt->fd(), state.generation));
  asyncState.task.await.needsBody = !bodyReady;
  asyncState.active = true;
  asyncState.usesSharedDecompressedBody = usesSharedDecompressedBody;
  asyncState.isChunked = isChunked;
  asyncState.consumedBytes = bodyReady ? consumedBytes : 0;
  asyncState.maxBodyBytes = perRouteMaxBodyBytes;
  asyncState.pendingResponse = internal::HttpResponseAccess::MakeUnallocated();

  // The request is not parsed again from now on: the expectations of the next one must be answered.
  state.expectationAnswered = false;

  // Keep header storage stable while async work runs so header string_views stay valid
  request.pinHeadStorage(asyncState.headBuffer);
  asyncState.task.bind(request);

  ctx.maintenance.refreshKeepAliveDeadline(cnxIt);
  resumeAsyncHandler(cnxIt);
  if (asyncState.active) {
    // Completes later: the router may be updated meanwhile.
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

  if (async.task.run()) {
    onAsyncHandlerCompleted(cnxIt);
  } else if (async.usesSharedDecompressedBody && !pinAsyncSharedBodyToConnectionStorage(state)) {
    state.requestDrainAndClose();
  }
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
  auto* asyncState = state.pAsyncState();
  assert(asyncState != nullptr);
  auto& async = *asyncState;
  assert(async.active);

  if (async.task.await.needsBody) {
    std::size_t consumedBytes = 0;
    // 100 Continue (if expected) was sent when the request was dispatched. The request starts the input buffer, where
    // its body is received.
    const BodyDecodeStatus status =
        decodeBodyIfReady(cnxIt, state.inBuffer, async.isChunked, false, async.maxBodyBytes, consumedBytes);
    if (status == BodyDecodeStatus::Error) {
      AbandonAsyncHandler(async);
      return;
    }
    if (status == BodyDecodeStatus::NeedMore) {
      return;
    }

    async.task.await.needsBody = false;
    async.consumedBytes = consumedBytes;
    if (!state.request._body.empty() && !maybeDecompressRequestBody(cnxIt, true)) {
      AbandonAsyncHandler(async);
      return;
    }
    state.installAggregatedBodyBridge();
    _pCtx->maintenance.trackBodyReception(state, true);

    if (async.task.await.reason == AsyncAwaitState::Reason::WaitingForBody) {
      resumeAsyncHandler(cnxIt);
      return;
    }
  }

  if (!async.pendingResponse.empty()) {
    tryFlushPendingAsyncResponse(cnxIt);
  }
}

bool Http1Pipeline::pinAsyncSharedBodyToConnectionStorage(ConnectionState& state) const {
  auto* asyncState = state.pAsyncState();
  if (asyncState == nullptr) {
    return true;
  }
  auto& async = *asyncState;
  if (!async.usesSharedDecompressedBody) {
    return true;
  }

  const std::string_view body = state.request._body;

  // Async shared-body pinning expects request body to reference shared decompressed storage
  [[maybe_unused]] const RawChars& sharedBody = _pCtx->sharedBuffers.decompressedBody;
  assert(body.empty() ||
         (sharedBody.data() <= body.data() && body.data() + body.size() <= sharedBody.data() + sharedBody.size()));

  state.bodyAndTrailersBuffer.assign(body.data(), body.size());
  state.request._body = std::string_view(state.bodyAndTrailersBuffer.data(), body.size());

  if (state.request._pBodyAccessBridge != nullptr) {
    state.bodyStreamContext.body = state.request._body;
  }

  async.usesSharedDecompressedBody = false;
  return true;
}

void Http1Pipeline::onAsyncHandlerCompleted(ConnectionIt cnxIt) {
  ConnectionState& state = _pCtx->connections.connectionState(cnxIt);
  AsyncHandlerState& async = *state.pAsyncState();
  async.pendingResponse = async.task.takeResponse(state.request);
  // Without its whole body, the request is answered once the rest of it is received (and drained).
  if (!async.task.await.needsBody) {
    tryFlushPendingAsyncResponse(cnxIt);
  }
}

void Http1Pipeline::tryFlushPendingAsyncResponse(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  AsyncHandlerState& async = *state.pAsyncState();

  assert(!async.task.await.needsBody);
  assert(!async.pendingResponse.empty());

  const CorsPolicy* pCorsPolicy =
      async.task.applyResponseMiddleware(ctx.requestPipeline(), state.request, async.pendingResponse);
  finalizeAndSendResponse(cnxIt, std::move(async.pendingResponse), pCorsPolicy);
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
        continue;
      }
#endif
      // The coroutine frame is kept until its deferred work completes: a coroutine created meanwhile cannot have the
      // same address, the handle identifies it.
      auto* asyncState = state.pAsyncState();
      if (asyncState != nullptr && asyncState->task.runs(cb.handle)) {
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
