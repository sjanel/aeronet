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
#include "aeronet/http-message-data.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-request-dispatch.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/internal/pending-updates.hpp"
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

bool SingleHttpServer::PauseReadingDuringAsyncHandler(ConnectionState& state) noexcept {
  const auto* asyncState = state.pAsyncState();
  if (asyncState == nullptr || !asyncState->holdsInput()) {
    return false;
  }
  // The input buffer holds the body (and the bytes) of the request of the async handler: it must neither grow nor move
  // until it completes. The next bytes are left in the socket (which also bounds them) and read once it completed,
  // see resumeInputAfterAsyncHandler().
  state.readPaused = true;
  return true;
}

bool SingleHttpServer::dispatchAsyncHandler(ConnectionIt cnxIt, const SharedAsyncRequestHandler& handler,
                                            bool bodyReady, bool isChunked, std::size_t consumedBytes,
                                            const CorsPolicy* pCorsPolicy,
                                            std::span<const ResponseMiddleware> responseMiddleware,
                                            std::size_t perRouteMaxBodyBytes) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  HttpRequestView& request = state.request;
  RequestTask<HttpResponse> task = (*handler)(request);

  if (!task.valid()) {
    static constexpr std::string_view kMessage = "Async handler inactive";
    log::error("Async path handler returned an invalid RequestTask for path {}", request.path());
    if (bodyReady) {
      HttpResponse resp(http::StatusCodeInternalServerError, kMessage);
      ApplyResponseMiddleware(request, resp, responseMiddleware, _router.globalResponseMiddleware(), _telemetry, false,
                              _callbacks.middlewareMetrics);
      finalizeAndSendResponseForHttp1(cnxIt, std::move(resp), pCorsPolicy);
    } else {
      emitSimpleError(cnxIt, http::StatusCodeInternalServerError, kMessage);
    }

    return false;
  }

  auto handle = task.release();
  assert(handle);

  auto& asyncState = state.ensureAsyncState(_connections.asyncHandlerStatePool());
  const std::string_view bodyView = state.request._body;
  bool usesSharedDecompressedBody = false;
  if (bodyReady && !bodyView.empty()) {
    const char* sharedBeg = _sharedBuffers.decompressedBody.data();
    const char* sharedEnd = sharedBeg + _sharedBuffers.decompressedBody.size();
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
  asyncState.pendingResponse = {};

  // The request is not parsed again from now on: the expectations of the next one must be answered.
  state.expectationAnswered = false;

  // Keep header storage stable while async work runs so header string_views stay valid
  state.request.pinHeadStorage(state, _connections.asyncHandlerStatePool());

  // Install the postCallback function for deferred work
  asyncState.postCallback = [this, fd = cnxIt->fd(), generation = state.generation](std::coroutine_handle<> handle,
                                                                                    std::function<void()> work) {
    postAsyncCallback(fd, generation, handle, std::move(work));
  };

  refreshKeepAliveDeadline(cnxIt);
  resumeAsyncHandler(cnxIt);
  if (asyncState.active) {
    // Completes later: the router may be updated meanwhile.
    asyncState.routeMayHaveChanged = true;
  }

  return true;
}

void SingleHttpServer::resumeAsyncHandler(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
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

void SingleHttpServer::AbandonAsyncHandler(AsyncHandlerState& async) noexcept {
  if (async.isAwaitingCallback()) {
    // The deferred work may still use the coroutine frame and the request: they are released when it completes.
    async.active = false;
    async.pendingResponse.reset();
  } else {
    async.clear();
  }
}

void SingleHttpServer::handleAsyncBodyProgress(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  auto* asyncState = state.pAsyncState();
  assert(asyncState != nullptr);
  auto& async = *asyncState;
  assert(async.active);

  if (async.needsBody) {
    std::size_t consumedBytes = 0;
    // 100 Continue (if expected) was sent when the request was dispatched.
    const BodyDecodeStatus status = decodeBodyIfReady(cnxIt, async.isChunked, false, async.maxBodyBytes, consumedBytes);
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

    if (_config.bodyReadTimeout.count() > 0) {
      if (state.waitingForBody) {
        assert(_connectionSweepState.pendingTimeoutConnections > 0U);
        --_connectionSweepState.pendingTimeoutConnections;
      }
      state.waitingForBody = false;
      state.bodyLastActivityMs = ConnectionState::kInactiveRelativeMs;
    }

    if (async.awaitReason == AsyncHandlerState::AwaitReason::WaitingForBody) {
      async.awaitReason = AsyncHandlerState::AwaitReason::None;
      resumeAsyncHandler(cnxIt);
      return;
    }
  }

  if (async.pendingResponse.has_value()) {
    tryFlushPendingAsyncResponse(cnxIt);
  }
}

bool SingleHttpServer::pinAsyncSharedBodyToConnectionStorage(ConnectionState& state) const {
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
  assert(body.empty() || (_sharedBuffers.decompressedBody.data() <= body.data() &&
                          body.data() + body.size() <=
                              _sharedBuffers.decompressedBody.data() + _sharedBuffers.decompressedBody.size()));

  state.bodyAndTrailersBuffer.assign(body.data(), body.size());
  state.request._body = std::string_view(state.bodyAndTrailersBuffer.data(), body.size());

  if (state.request._pBodyAccessBridge != nullptr) {
    state.bodyStreamContext.body = state.request._body;
  }

  async.usesSharedDecompressedBody = false;
  return true;
}

void SingleHttpServer::onAsyncHandlerCompleted(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
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
  HttpResponse resp(HttpMessage::Check::No);  // do not allocate memory yet
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

void SingleHttpServer::tryFlushPendingAsyncResponse(ConnectionIt cnxIt) {
  ConnectionState& state = _connections.connectionState(cnxIt);
  auto* asyncState = state.pAsyncState();
  if (asyncState == nullptr) {
    return;
  }
  auto& async = *asyncState;

  assert(!async.needsBody);
  assert(async.pendingResponse.has_value());

  auto middlewareSpan = std::span<const ResponseMiddleware>(
      static_cast<const ResponseMiddleware*>(async.responseMiddleware), async.responseMiddlewareCount);
  const CorsPolicy* pCorsPolicy = async.corsPolicy;
  if (async.routeMayHaveChanged) {
    // The router may have been updated since the handler was dispatched, possibly destroying the metadata of its route:
    // look it up again.
    const Router::RoutingResult routingResult = _router.match(state.request.method(), state.request.path());
    middlewareSpan = routingResult.postMiddlewareRange();
    pCorsPolicy = routingResult.corsPolicy();
  }
  ApplyResponseMiddleware(state.request, *async.pendingResponse, middlewareSpan, _router.globalResponseMiddleware(),
                          _telemetry, false, _callbacks.middlewareMetrics);
  finalizeAndSendResponseForHttp1(cnxIt, std::move(*async.pendingResponse), pCorsPolicy);
  state.inBuffer.erase_front(async.consumedBytes);
  async.clear();
  state.lastActivity = std::chrono::steady_clock::now();
  refreshKeepAliveDeadline(cnxIt);
}

void SingleHttpServer::resumeInputAfterAsyncHandler(NativeHandle fd) {
  auto cnxIt = _connections.iterator(fd);
  ConnectionState* pState = _connections.pConnectionState(cnxIt);
  // Serve the requests pipelined behind the completed one: they were already read, no event will report them.
  if (!pState->inBuffer.empty()) {
    (void)processHttp1Requests(cnxIt);
    cnxIt = _connections.iterator(fd);
    pState = _connections.pConnectionState(cnxIt);
  }
  // Then read the bytes left in the socket while reading was paused: edge-triggered polling does not report them again.
  if (pState->readPaused && !pState->isAnyCloseRequested()) {
    pState->readPaused = false;
    if (handleReadableClient(cnxIt, false) == CloseStatus::Close) {
      const auto finalIt = _connections.iterator(fd);
      if (IsValid(_connections, finalIt)) {
        closeConnection(finalIt);
      }
    }
  }
}

void SingleHttpServer::processAsyncCallbacks() {
  vector<internal::PendingUpdates::AsyncCallback> callbacks;
  {
    std::scoped_lock lock(_updates.lock);
    callbacks.swap(_updates.asyncCallbacks);
    _updates.hasAsyncCallbacks.store(false, std::memory_order_release);
  }

  for (auto& cb : callbacks) {
    try {
      auto it = _connections.findConnection(cb.connectionFd, cb.connectionGeneration);
      if (it == _connections.end()) {
        // The connection was closed while the work was running: its coroutine was kept for it, release it now.
        _connections.releaseOrphanedAsyncTask(cb.connectionGeneration, cb.handle, _config.maxCachedConnections);
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
        it = _connections.findConnection(cb.connectionFd, cb.connectionGeneration);
        if (it == _connections.end()) {
          _connections.releaseOrphanedAsyncTask(cb.connectionGeneration, cb.handle, _config.maxCachedConnections);
          continue;
        }
      }

      ConnectionState& state = _connections.connectionState(it);
#ifdef AERONET_ENABLE_HTTP2
      // For HTTP/2 connections, delegate to the protocol handler which tracks per-stream async state.
      if (state.protocol == ProtocolType::Http2 && state.protocolHandler != nullptr) {
        auto* pH2Handler = static_cast<http2::Http2ProtocolHandler*>(state.protocolHandler.get());
        if (pH2Handler->resumeAsyncTaskByHandle(cb.handle)) {
          // Flush any pending output generated by the completed async handler
          if (pH2Handler->hasPendingOutput()) {
            flushOutbound(it);
          }
          // Deferred work that outlived keepAliveTimeout leaves a stale idle deadline behind, exactly as
          // a slow synchronous handler does (HTTP/1 goes through tryFlushPendingAsyncResponse instead).
          restartKeepAliveIdleWindow(cb.connectionFd);
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
          resumeAsyncHandler(it);
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
      it = _connections.findConnection(cb.connectionFd, cb.connectionGeneration);
      if (it != _connections.end() && _connections.connectionState(it).canCloseConnectionForDrain()) {
        closeConnection(it);
      }
    } catch (const std::exception& ex) {
      log::error("Exception processing async callback for fd # {}: {}", static_cast<uintptr_t>(cb.connectionFd),
                 ex.what());
    } catch (...) {
      log::error("Unknown exception processing async callback for fd # {}", static_cast<uintptr_t>(cb.connectionFd));
    }
  }
}

void SingleHttpServer::postAsyncCallback(NativeHandle connectionFd, uint32_t connectionGeneration,
                                         std::coroutine_handle<> handle, std::function<void()> work) {
  std::scoped_lock lock(_updates.lock);
  _updates.asyncCallbacks.emplace_back(connectionFd, connectionGeneration, handle, std::move(work));
  _updates.hasAsyncCallbacks.store(true, std::memory_order_release);
  // Still under the lock: once it is released, the server may be destroyed (see waitForOrphanedAsyncWork()).
  _lifecycle.wakeupFd.send();
}

void SingleHttpServer::waitForOrphanedAsyncWork() {
  if (!_lifecycle.isIdle()) {
    // Still run by another thread, which owns the connections.
    return;
  }
  // Deferred work still running uses the coroutine frame and the request kept for it, and completes by posting to this
  // server: wait for it.
  static constexpr auto kLogPeriod = std::chrono::seconds{5};
  auto nextLog = std::chrono::steady_clock::now() + kLogPeriod;
  while (_connections.hasOrphanedConnectionStates()) {
    if (_updates.hasAsyncCallbacks.load(std::memory_order_acquire)) {
      processAsyncCallbacks();
      continue;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= nextLog) {
      log::warn("Waiting for the deferred work still running for {} closed connection(s)",
                _connections.nbOrphanedConnectionStates());
      nextLog = now + kLogPeriod;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

}  // namespace aeronet
