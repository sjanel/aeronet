#include "aeronet/internal/server-context.hpp"

#include <mutex>
#include <string_view>
#include <utility>

#include "aeronet/adaptive-poll-timeout.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/request-metrics.hpp"
#include "aeronet/router.hpp"
#include "aeronet/string-trim.hpp"
#include "tunnel-manager.hpp"  // IWYU pragma: keep (destruction of ServerContext::tunnels)

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
#include <atomic>
#include <coroutine>

#include "aeronet/async-resume-poster.hpp"
#endif

namespace aeronet::internal {

PollTimeoutPolicy MakePollTimeoutPolicy(const HttpServerConfig& config) {
  return PollTimeoutPolicy{
      .baseTimeout = config.pollInterval,
      .minFactor = config.pollIntervalMinFactor,
      .maxFactor = config.pollIntervalMaxFactor,
  };
}

// The default globalHeaders ("server: aeronet") never contain the separator that makes ConcatenatedHeaders throw.
// NOLINTNEXTLINE(bugprone-exception-escape)
ServerContext::ServerContext() noexcept = default;

ServerContext::ServerContext(HttpServerConfig cfg, Router rtr)
    : config(std::move(cfg)),
      compressionState(config.compression),
      eventLoop(MakePollTimeoutPolicy(config)),
      router(std::move(rtr)),
      telemetry(config.telemetry) {}

ServerContext::ServerContext(const ServerContext& other, [[maybe_unused]] CopySetupTag tag)
    : callbacks(other.callbacks),
      updates(other.updates),
      config(other.config),
      compressionState(config.compression),
      // do not copy the decompression state, we just use our own.
      // the event loop is created by SingleHttpServer::initListener().
      router(other.router),
      telemetry(config.telemetry) {
#ifdef AERONET_ENABLE_OPENSSL
  // Inherits the shared ticket key store for MultiHttpServer fan-out.
  tls.shareTicketKeyStore(other.tls.sharedTicketKeyStore());
#endif
}

// The date header cache, the access log writer and the tunnels (null when not running) are not moved: the cache is
// refreshed and the writer created again by the next run.
ServerContext::ServerContext(ServerContext&& other) noexcept
    : stats(std::exchange(other.stats, {})),
      callbacks(std::move(other.callbacks)),
      updates(std::move(other.updates)),
      config(std::move(other.config)),
      compressionState(std::move(other.compressionState)),
      decompressionState(std::move(other.decompressionState)),
#ifdef AERONET_ENABLE_WEBSOCKET
      webSocketDeflateStreams(std::move(other.webSocketDeflateStreams)),
#endif
      eventLoop(std::move(other.eventLoop)),
      lifecycle(std::move(other.lifecycle)),
      router(std::move(other.router)),
      connections(std::move(other.connections)),
      sharedBuffers(std::move(other.sharedBuffers)),
      telemetry(std::move(other.telemetry)),
      io(std::move(other.io)),
      maintenance(std::move(other.maintenance)),
      http1(std::move(other.http1))
#ifdef AERONET_ENABLE_OPENSSL
      ,
      tls(std::move(other.tls))
#endif
{
  compressionState.pCompressionConfig = &config.compression;
  bindComponents();
}

ServerContext& ServerContext::operator=(ServerContext&& other) noexcept {
  if (this != &other) {
    stats = std::exchange(other.stats, {});
    callbacks = std::move(other.callbacks);
    updates = std::move(other.updates);
    config = std::move(other.config);

    compressionState = std::move(other.compressionState);
    compressionState.pCompressionConfig = &config.compression;

    decompressionState = std::move(other.decompressionState);
#ifdef AERONET_ENABLE_WEBSOCKET
    webSocketDeflateStreams = std::move(other.webSocketDeflateStreams);
#endif
    eventLoop = std::move(other.eventLoop);
    lifecycle = std::move(other.lifecycle);
    router = std::move(other.router);
    connections = std::move(other.connections);
    sharedBuffers = std::move(other.sharedBuffers);
    telemetry = std::move(other.telemetry);
    io = std::move(other.io);
    maintenance = std::move(other.maintenance);
    http1 = std::move(other.http1);
#ifdef AERONET_ENABLE_OPENSSL
    tls = std::move(other.tls);
#endif
    bindComponents();
  }
  return *this;
}

ServerContext::~ServerContext() = default;

void ServerContext::bindComponents() noexcept {
  io.bind(*this);
  maintenance.bind(*this);
  http1.bind(*this);
#ifdef AERONET_ENABLE_OPENSSL
  tls.bind(*this);
#endif
}

void ServerContext::emitRequestMetrics(const HttpRequestView& request, http::StatusCode status) {
  std::string_view clientIp = "-";

  if (config.accessLog.useForwardedFor) {
    const auto xff = request.headerValueOrEmpty("x-forwarded-for");
    if (!xff.empty()) {
      // Use the first (leftmost) IP from X-Forwarded-For
      const auto comma = xff.find(',');
      clientIp = TrimOws(xff.substr(0, comma));
    }
  }

  if (clientIp == "-") {
    const auto requestClientIp = request.clientAddress();
    if (!requestClientIp.empty()) {
      clientIp = requestClientIp;
    }
  }

  RequestMetrics metrics{
      status,
      request.method(),
      request.version(),
      // The request is counted already: when its head is parsed over HTTP/1.x, when it completes over HTTP/2.
      request._pConnectionInfo->requestsServed > 1,
      request.path(),
      clientIp,
      request.headerValueOrEmpty("user-agent"),
      request._body.size(),
      0,
      connections.now - request.reqStart(),
  };

  if (accessLog) {
    accessLog.log(metrics);
  }

  if (callbacks.metrics) {
    callbacks.metrics(metrics);
  }
}

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
void ServerContext::PostAsyncResume(const AsyncResumePoster& poster, std::coroutine_handle<> handle) {
  auto& ctx = *static_cast<ServerContext*>(poster.pContext);
  std::scoped_lock lock(ctx.updates.lock);
  ctx.updates.asyncCallbacks.emplace_back(poster.connectionFd, poster.connectionGeneration, handle);
  ctx.updates.hasAsyncCallbacks.store(true, std::memory_order_release);
  // Still under the lock: once it is released, the server may be destroyed (see
  // SingleHttpServer::waitForOrphanedAsyncWork()).
  ctx.lifecycle.wakeupFd.send();
}
#endif

}  // namespace aeronet::internal
