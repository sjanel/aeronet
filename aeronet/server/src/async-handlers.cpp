#include <atomic>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <thread>
#include <utility>

#include "aeronet/async-handler-state.hpp"
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
  RequestTask<HttpResponse> task = RequestPipeline::CallAsyncHandler(*handler, request);

  if (!task.valid()) {
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

  auto handle = task.release();
  assert(handle);

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

  // The coroutine may use the captures of the handler: keep it alive even if the router replaces it meanwhile.
  asyncState.handlerKeepAlive = handler;
  asyncState.active = true;
  asyncState.handle = std::move(handle);
  asyncState.awaitReason = AsyncHandlerState::AwaitReason::None;
  asyncState.needsBody = !bodyReady;
  asyncState.usesSharedDecompressedBody = usesSharedDecompressedBody;
  asyncState.isChunked = isChunked;
  asyncState.routeMayHaveChanged = false;
  asyncState.consumedBytes = bodyReady ? consumedBytes : 0;
  asyncState.corsPolicy = pCorsPolicy;
  asyncState.responseMiddleware = responseMiddleware.data();
  asyncState.responseMiddlewareCount = static_cast<uint32_t>(responseMiddleware.size());
  asyncState.maxBodyBytes = perRouteMaxBodyBytes;
  asyncState.pendingResponse = internal::HttpResponseAccess::MakeUnallocated();

  // The request is not parsed again from now on: the expectations of the next one must be answered.
  state.expectationAnswered = false;

  // Keep header storage stable while async work runs so header string_views stay valid
  state.request.pinHeadStorage(asyncState.headBuffer);

  // Install the postCallback function for deferred work
  asyncState.postCallback = [pCtx = &ctx, fd = cnxIt->fd(), generation = state.generation](
                                std::coroutine_handle<> handle, std::function<void()> work) {
    pCtx->postAsyncCallback(fd, generation, handle, std::move(work));
  };

  ctx.maintenance.refreshKeepAliveDeadline(cnxIt);
  resumeAsyncHandler(cnxIt);
  if (asyncState.active) {
    // Completes later: the router may be updated meanwhile.
    asyncState.routeMayHaveChanged = true;
  }

  return true;
}

void Http1Pipeline::resumeAsyncHandler(ConnectionIt cnxIt) {
  ConnectionState& state = _pCtx->connections.connectionState(cnxIt);
  auto* asyncState = state.pAsyncState();
  assert(asyncState != nullptr);
  auto& async = *asyncState;
  if (!async.active || !async.handle) {
    return;
  }

  while (async.handle && !async.handle.done()) {
    async.awaitReason = AsyncHandlerState::AwaitReason::None;
    async.handle.resume();
    if (async.awaitReason != AsyncHandlerState::AwaitReason::None) {
      if (async.usesSharedDecompressedBody && !pinAsyncSharedBodyToConnectionStorage(state)) {
        state.requestDrainAndClose();
      }
      return;
    }
    // Suspended by an awaitable that does not need the event loop (std::suspend_always for instance): resume it.
  }

  if (async.handle && async.handle.done()) {
    onAsyncHandlerCompleted(cnxIt);
  }
}

void Http1Pipeline::AbandonAsyncHandler(AsyncHandlerState& async) noexcept {
  if (async.isAwaitingCallback()) {
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

  if (async.needsBody) {
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

    async.needsBody = false;
    async.consumedBytes = consumedBytes;
    if (!state.request._body.empty() && !maybeDecompressRequestBody(cnxIt, true)) {
      AbandonAsyncHandler(async);
      return;
    }
    state.installAggregatedBodyBridge();
    _pCtx->maintenance.trackBodyReception(state, true);

    if (async.awaitReason == AsyncHandlerState::AwaitReason::WaitingForBody) {
      async.awaitReason = AsyncHandlerState::AwaitReason::None;
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
  auto* asyncState = state.pAsyncState();
  if (asyncState == nullptr) {
    return;
  }
  auto& async = *asyncState;
  if (!async.handle) {
    return;
  }

  auto rawHandle = async.handle.release();
  auto typedHandle = std::coroutine_handle<RequestTask<HttpResponse>::promise_type>::from_address(rawHandle.address());
  bool fromException = false;
  HttpResponse resp = internal::HttpResponseAccess::MakeUnallocated();
  try {
    resp = std::move(typedHandle.promise().consume_result());
  } catch (const std::exception& ex) {
    fromException = true;
    log::error("Exception in async path handler: {}", ex.what());
    resp = HttpResponse(http::StatusCodeInternalServerError);
  } catch (...) {
    fromException = true;
    log::error("Unknown exception in async path handler");
    resp = HttpResponse(http::StatusCodeInternalServerError);
  }
  typedHandle.destroy();
  async.handlerKeepAlive.reset();
  async.pendingResponse = std::move(resp);

  if (async.needsBody) {
    if (fromException) {
      // Body will still be drained before response is flushed; nothing else to do here.
    }
  } else {
    tryFlushPendingAsyncResponse(cnxIt);
  }
}

void Http1Pipeline::tryFlushPendingAsyncResponse(ConnectionIt cnxIt) {
  ServerContext& ctx = *_pCtx;
  ConnectionState& state = ctx.connections.connectionState(cnxIt);
  auto* asyncState = state.pAsyncState();
  if (asyncState == nullptr) {
    return;
  }
  auto& async = *asyncState;

  assert(!async.needsBody);
  assert(!async.pendingResponse.empty());

  auto middlewareSpan = std::span<const ResponseMiddleware>(
      static_cast<const ResponseMiddleware*>(async.responseMiddleware), async.responseMiddlewareCount);
  const CorsPolicy* pCorsPolicy = async.corsPolicy;
  if (async.routeMayHaveChanged) {
    // The router may have been updated since the handler was dispatched, possibly destroying the metadata of its route:
    // look it up again.
    const Router::RoutingResult routingResult = ctx.router.match(state.request.method(), state.request.path());
    middlewareSpan = routingResult.postMiddlewareRange();
    pCorsPolicy = routingResult.corsPolicy();
  }
  ctx.requestPipeline().applyResponseMiddleware(state.request, async.pendingResponse, middlewareSpan, false);
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

      // Execute any pre-resume work
      if (cb.work) {
        try {
          cb.work();
        } catch (const std::exception& ex) {
          log::error("Exception in async callback work: {}", ex.what());
        } catch (...) {
          log::error("Unknown exception in async callback work");
        }
        it = connections.findConnection(cb.connectionFd, cb.connectionGeneration);
        if (it == connections.end()) {
          connections.releaseOrphanedAsyncTask(cb.connectionGeneration, cb.handle, _ctx.config.maxCachedConnections);
          continue;
        }
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
      if (asyncState != nullptr && asyncState->handle == cb.handle) {
        if (asyncState->active) {
          asyncState->awaitReason = AsyncHandlerState::AwaitReason::None;
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
