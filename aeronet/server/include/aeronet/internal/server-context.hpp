#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include "aeronet/access-log-writer.hpp"
#include "aeronet/adaptive-poll-timeout.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/http-codec.hpp"
#include "aeronet/http-request-dispatch.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/internal/connection-io.hpp"
#include "aeronet/internal/connection-maintenance.hpp"
#include "aeronet/internal/connection-storage.hpp"
#include "aeronet/internal/date-header-cache.hpp"
#include "aeronet/internal/http1-pipeline.hpp"
#include "aeronet/internal/lifecycle.hpp"
#include "aeronet/internal/pending-updates.hpp"
#include "aeronet/internal/shared-buffers.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-callbacks.hpp"
#include "aeronet/single-writer-counter.hpp"
#include "aeronet/tracing/tracer.hpp"

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <coroutine>

#include "aeronet/native-handle.hpp"
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/internal/tls-acceptor.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
#include "aeronet/websocket-deflate.hpp"
#endif

namespace aeronet::internal {

class TunnelManager;

// Updated by the event loop, read by SingleHttpServer::stats() from any thread.
struct ServerStatsInternal {
  SingleWriterCounter<uint64_t> totalBytesQueued;
  SingleWriterCounter<uint64_t> totalBytesWrittenImmediate;
  SingleWriterCounter<uint64_t> totalBytesWrittenFlush;
  SingleWriterCounter<uint64_t> deferredWriteEvents;
  SingleWriterCounter<uint64_t> flushCycles;
  SingleWriterCounter<uint64_t> epollModFailures;
  SingleWriterCounter<std::size_t> maxConnectionOutboundBuffer;
  SingleWriterCounter<uint64_t> totalRequestsServed;
  SingleWriterCounter<uint64_t> zerocopySends;
  SingleWriterCounter<uint64_t> zerocopyBytes;
  SingleWriterCounter<uint64_t> zerocopyCopiedSends;
  SingleWriterCounter<uint64_t> zerocopyNoBufferSpaceFallbacks;
  SingleWriterCounter<uint64_t> zerocopyRetainLimitFallbacks;
  SingleWriterCounter<uint64_t> zerocopyResetConnections;
};

// The user callbacks of a server, used by its event loop only (see SingleHttpServer::submitCallbacksUpdate()).
struct ServerCallbacks {
  ParserErrorCallback parserErr;
  MetricsCallback metrics;
  MiddlewareMetricsCallback middlewareMetrics;
#ifdef AERONET_ENABLE_OPENSSL
  TlsHandshakeCallback tlsHandshake;
#endif
  ExpectationHandler expectation;
};

[[nodiscard]] PollTimeoutPolicy MakePollTimeoutPolicy(const HttpServerConfig& config);

// What the parts of a SingleHttpServer share: its configuration and runtime state, and the components serving its
// connections. Owned by the server (which keeps the lifecycle, the listener, the event loop dispatch and the updates),
// it is the single reference through which the components reach each other, as do the objects working on the
// connections (CONNECT tunnels, streaming response writers, HTTP/2 tunnel bridges).
// Each component points back to it: its copy and move operations point the components of the result to it.
struct ServerContext {
  // Default configuration and router, without event loop (created by SingleHttpServer::initListener()).
  ServerContext() noexcept;

  ServerContext(HttpServerConfig cfg, Router rtr);

  // The configuration, the router, the callbacks, the pending updates and the shared session ticket key store of
  // 'other', without any runtime state (no event loop).
  struct CopySetupTag {};
  ServerContext(const ServerContext& other, CopySetupTag);

  ServerContext(const ServerContext&) = delete;
  ServerContext(ServerContext&& other) noexcept;
  ServerContext& operator=(const ServerContext&) = delete;
  ServerContext& operator=(ServerContext&& other) noexcept;

  ~ServerContext();

  // The steps of the request pipeline shared by HTTP/1.1 and HTTP/2.
  [[nodiscard]] RequestPipeline requestPipeline() noexcept {
    return {router, telemetry, callbacks.middlewareMetrics, sharedBuffers.buf};
  }

  // Bytes written to all the connections so far, counted toward the fairness budget of the events that wrote them.
  [[nodiscard]] uint64_t totalBytesWritten() const noexcept {
    return stats.totalBytesWrittenImmediate.load() + stats.totalBytesWrittenFlush.load();
  }

  // Populates and invokes the metrics callback and the access log for a completed request (HTTP/1.x or HTTP/2).
  void emitRequestMetrics(const HttpRequestView& request, http::StatusCode status);

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Post an async callback to be processed in the event loop, then resume the coroutine.
  // Called from background threads via ConnectionState::asyncState.postCallback.
  void postAsyncCallback(NativeHandle connectionFd, uint32_t connectionGeneration, std::coroutine_handle<> handle);
#endif

  ServerStatsInternal stats;
  ServerCallbacks callbacks;
  PendingUpdates updates;
  HttpServerConfig config;
  CompressionState compressionState;
  DecompressionState decompressionState;
#ifdef AERONET_ENABLE_WEBSOCKET
  // permessage-deflate streams of the WebSocket connections, for the directions negotiated without context takeover.
  // Declared before the connections, whose handlers use them.
  websocket::SharedDeflateStreams webSocketDeflateStreams;
#endif
  EventLoop eventLoop;
  Lifecycle lifecycle;
  DateHeaderCache dateHeader;
  Router router;
  ConnectionStorage connections;
  SharedBuffers sharedBuffers;
  // Telemetry context - one per SingleHttpServer instance (no global singletons)
  tracing::TelemetryContext telemetry;
  // Structured access log writer - one per event-loop thread.
  AccessLogWriter accessLog;
  // CONNECT tunnels: created by the first CONNECT request of a run, and destroyed when the event loop stops (always
  // null when the server does not run, so the special members of the server need not care).
  std::unique_ptr<TunnelManager> tunnels;

  ConnectionIo io{*this};
  ConnectionMaintenance maintenance{*this};
  Http1Pipeline http1{*this};
#ifdef AERONET_ENABLE_OPENSSL
  TlsAcceptor tls{*this};
#endif

 private:
  // Points the components to this context.
  void bindComponents() noexcept;
};

}  // namespace aeronet::internal
