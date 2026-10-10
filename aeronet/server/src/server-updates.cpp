#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include "aeronet/accept-encoding-negotiation.hpp"
#include "aeronet/access-log-writer.hpp"
#include "aeronet/connection-state.hpp"
#include "aeronet/event-loop.hpp"
#include "aeronet/http-message.hpp"
#include "aeronet/http-request-view.hpp"
#include "aeronet/http-response-writer.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/log.hpp"
#include "aeronet/middleware.hpp"
#include "aeronet/router-update-proxy.hpp"
#include "aeronet/router.hpp"
#include "aeronet/single-http-server.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/vector.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-context.hpp"
#include "aeronet/tls-handshake-callback.hpp"
#endif

#ifdef AERONET_ENABLE_HTTP2
#include "aeronet/http2-protocol-handler.hpp"
#endif

namespace aeronet {

namespace {

// Snapshot of immutable HttpServerConfig fields that require socket rebind or structural reinitialization.
// These fields are captured before allowing config updates and silently restored afterward to prevent
// runtime modification of settings that cannot be changed without recreating the server.
class ImmutableConfigSnapshot {
 public:
  explicit ImmutableConfigSnapshot(const HttpServerConfig& cfg)
      : _nbThreads(cfg.nbThreads), _port(cfg.port), _reusePort(cfg.reusePort), _telemetry(cfg.telemetry) {}

  void restore(HttpServerConfig& cfg) {
    if (cfg.nbThreads != _nbThreads) [[unlikely]] {
      cfg.nbThreads = _nbThreads;
      log::warn("Attempted to modify immutable HttpServerConfig.nbThreads at runtime; change ignored");
    }
    if (cfg.port != _port) [[unlikely]] {
      cfg.port = _port;
      log::warn("Attempted to modify immutable HttpServerConfig.port at runtime; change ignored");
    }
    if (cfg.reusePort != _reusePort) [[unlikely]] {
      cfg.reusePort = _reusePort;
      log::warn("Attempted to modify immutable HttpServerConfig.reusePort at runtime; change ignored");
    }
    if (cfg.telemetry != _telemetry) [[unlikely]] {
      cfg.telemetry = std::move(_telemetry);
      log::warn("Attempted to modify immutable HttpServerConfig.telemetry at runtime; change ignored");
    }
  }

 private:
  uint16_t _nbThreads;
  uint16_t _port;
  bool _reusePort;
  TelemetryConfig _telemetry;
};

}  // namespace

RouterUpdateProxy SingleHttpServer::router() {
  return {[this](std::function<void(Router&)> updater) {
            auto completion = std::make_shared<std::promise<std::exception_ptr>>();
            auto future = completion->get_future();
            this->submitRouterUpdate(std::move(updater), std::move(completion));
            if (auto ex = future.get()) {
              std::rethrow_exception(ex);
            }
          },
          [this] -> Router& { return _router; }};
}

void SingleHttpServer::setParserErrorCallback(ParserErrorCallback cb) {
  submitCallbacksUpdate([cb = std::move(cb)](Callbacks& callbacks) { callbacks.parserErr = cb; });
}

void SingleHttpServer::setMetricsCallback(MetricsCallback cb) {
  submitCallbacksUpdate([cb = std::move(cb)](Callbacks& callbacks) { callbacks.metrics = cb; });
}

#ifdef AERONET_ENABLE_OPENSSL
void SingleHttpServer::setTlsHandshakeCallback(TlsHandshakeCallback cb) {
  submitCallbacksUpdate([cb = std::move(cb)](Callbacks& callbacks) { callbacks.tlsHandshake = cb; });
}
#endif

void SingleHttpServer::setExpectationHandler(ExpectationHandler handler) {
  submitCallbacksUpdate([handler = std::move(handler)](Callbacks& callbacks) { callbacks.expectation = handler; });
}

void SingleHttpServer::setMiddlewareMetricsCallback(MiddlewareMetricsCallback cb) {
  submitCallbacksUpdate([cb = std::move(cb)](Callbacks& callbacks) { callbacks.middlewareMetrics = cb; });
}

void SingleHttpServer::submitCallbacksUpdate(std::function<void(Callbacks&)> updater) {
  if (_lifecycle.isEventLoopThread()) {
    // From a handler or a callback: the event loop is the thread using the callbacks.
    updater(_callbacks);
    return;
  }

  // While running, the change is applied by the event loop, and this call waits for it: the new callback is then used
  // for everything happening after this call (the event loop may otherwise still process, in its current iteration, a
  // connection initiated after it).
  auto applied = std::make_shared<std::atomic<bool>>(false);
  {
    std::scoped_lock lock(_updates.lock);
    if (_lifecycle.isIdle()) {
      applyQueuedCallbacksUpdates();
      updater(_callbacks);
      return;
    }
    _callbacksUpdates.emplace_back([updater = std::move(updater), applied](Callbacks& callbacks) {
      // Released even if the updater throws, so that the waiting caller does not wait for the server to stop.
      const auto release = [&applied] noexcept { applied->store(true, std::memory_order_release); };
      try {
        updater(callbacks);
      } catch (...) {
        release();
        throw;
      }
      release();
    });
    _hasCallbacksUpdates.store(true, std::memory_order_release);
  }
  _lifecycle.wakeupFd.send();

  while (!applied->load(std::memory_order_acquire)) {
    {
      std::scoped_lock lock(_updates.lock);
      if (_lifecycle.isIdle()) {
        // The event loop exited before applying it: no thread uses the callbacks anymore.
        applyQueuedCallbacksUpdates();
        return;
      }
    }
    // Bounded wait, so that the exit of the event loop is noticed even if the update is never applied by it.
    std::this_thread::sleep_for(std::chrono::microseconds{100});
  }
}

void SingleHttpServer::applyQueuedCallbacksUpdates() {
  // Changes still queued (posted just before the event loop exited) first, so that the latest one wins.
  for (auto& pendingUpdater : _callbacksUpdates) {
    pendingUpdater(_callbacks);
  }
  _callbacksUpdates.clear();
  _hasCallbacksUpdates.store(false, std::memory_order_relaxed);
}

void SingleHttpServer::postConfigUpdate(std::function<void(HttpServerConfig&)> updater) {
  {
    std::scoped_lock lock(_updates.lock);
    // Wrap user's updater with immutability enforcement: apply user changes then restore immutable fields.
    // The snapshot is taken when applying the update, by the event loop: the config cannot be read from the calling
    // thread, the event loop possibly applying a previous update at the same time.
    struct WrappedUpdater {
      void operator()(HttpServerConfig& cfg) const {
        ImmutableConfigSnapshot snapshot(cfg);
        userUpdater(cfg);
        snapshot.restore(cfg);
      }

      std::function<void(HttpServerConfig&)> userUpdater;
    };

    _updates.config.emplace_back(WrappedUpdater{std::move(updater)});
    _updates.hasConfig.store(true, std::memory_order_release);
  }
  _lifecycle.wakeupFd.send();
}

void SingleHttpServer::postRouterUpdate(std::function<void(Router&)> updater) {
  submitRouterUpdate(std::move(updater), {});
}

void SingleHttpServer::submitRouterUpdate(std::function<void(Router&)> updater,
                                          std::shared_ptr<std::promise<std::exception_ptr>> completion) {
  auto wrappedUpdater = [fn = std::move(updater), completionPtr = std::move(completion)](Router& router) mutable {
    try {
      fn(router);
      if (completionPtr) {
        completionPtr->set_value(nullptr);
      }
    } catch (const std::exception& ex) {
      if (completionPtr) {
        completionPtr->set_value(std::current_exception());
      } else {
        log::error("Exception while applying posted router update: {}", ex.what());
      }
    } catch (...) {
      assert(completionPtr == nullptr);
      log::error("Unknown exception while applying posted router update");
    }
  };

  {
    std::scoped_lock lock(_updates.lock);
    // beginStartup() takes this same lock before publishing Starting. A direct update
    // therefore completes before startup begins; every later update is queued for the
    // event-loop thread and clamped there.
    if (_lifecycle.isIdle()) {
      wrappedUpdater(_router);
      return;
    }
    _updates.router.emplace_back(std::move(wrappedUpdater));
    _updates.hasRouter.store(true, std::memory_order_release);
  }
  _lifecycle.wakeupFd.send();
}

namespace {

// The commit and the rollback of a config update rely on these swaps never throwing.
static_assert(std::is_nothrow_swappable_v<HttpServerConfig>);
static_assert(std::is_nothrow_swappable_v<AccessLogWriter>);

auto TakePendingUpdates(std::mutex& mutex, auto& vec, std::atomic<bool>& flag) {
  std::remove_reference_t<decltype(vec)> pendingUpdates;
  std::scoped_lock lock(mutex);
  pendingUpdates.swap(vec);
  flag.store(false, std::memory_order_release);
  return pendingUpdates;
}

void ApplyPendingUpdates(std::mutex& mutex, auto& vec, std::atomic<bool>& flag, auto& objToUpdate,
                         std::string_view name) {
  for (auto& updater : TakePendingUpdates(mutex, vec, flag)) {
    try {
      updater(objToUpdate);
    } catch (const std::exception& ex) {
      log::error("Exception while applying posted {} update: {}", name, ex.what());
    } catch (...) {
      log::error("Unknown exception while applying posted {} update", name);
    }
  }
}

}  // namespace

void SingleHttpServer::applyPendingConfigUpdates() {
#ifdef AERONET_ENABLE_OPENSSL
  bool tlsContextRebuilt = false;
#endif
  for (auto& updater : TakePendingUpdates(_updates.lock, _updates.config, _updates.hasConfig)) {
    try {
      // Each update is applied atomically: if the updater, the validation or the construction of the objects built from
      // the new configuration (TLS context, access log writer) throws, the previous configuration is restored.
      HttpServerConfig previousConfig(_config);
      try {
        updater(_config);
        _config.validate();

#ifdef AERONET_ENABLE_OPENSSL
        // Existing connections keep the context they were created from (ConnectionState::tlsContextKeepAlive).
        std::shared_ptr<TlsContext> newTlsContext;
        const bool tlsChanged = _config.tls != previousConfig.tls;
        if (tlsChanged && _config.tls.enabled) {
          newTlsContext = std::make_shared<TlsContext>(_config.tls, ticketKeyStoreForNewTlsContext(previousConfig.tls));
        }
#endif
        AccessLogWriter newAccessLog;
        const bool accessLogChanged = _config.accessLog != previousConfig.accessLog;
        if (accessLogChanged) {
          newAccessLog = AccessLogWriter(_config.accessLog);
        }

        // Commit - nothing can throw from here.
#ifdef AERONET_ENABLE_OPENSSL
        if (tlsChanged) {
          _tls.ctxHolder = std::move(newTlsContext);
          tlsContextRebuilt = true;
        }
#endif
        if (accessLogChanged) {
          // The previous writer, swapped into newAccessLog, flushes its buffered lines when destroyed.
          std::swap(_accessLog, newAccessLog);
        }
      } catch (...) {
        // Swap rather than assign: the rejected configuration is then destroyed with previousConfig, which scrubs its
        // TLS secrets (a move assignment would free their buffers without scrubbing them).
        std::swap(_config, previousConfig);
        throw;
      }
    } catch (const std::exception& ex) {
      log::error("Posted config update rejected, previous configuration kept: {}", ex.what());
    } catch (...) {
      log::error("Posted config update rejected (unknown exception), previous configuration kept");
    }
  }

#ifdef AERONET_ENABLE_OPENSSL
  // A context built by this batch has just read its files. Otherwise, reload the ones that changed on disk (in-place
  // certificate rotation, OCSP response or CRL refresh, Kubernetes secret update) even if their paths did not change.
  if (!tlsContextRebuilt && _tls.ctxHolder) {
    reloadChangedTlsFiles();
  }
#endif
}

#ifdef AERONET_ENABLE_OPENSSL
std::shared_ptr<TlsTicketKeyStore> SingleHttpServer::ticketKeyStoreForNewTlsContext(
    const TLSConfig& currentContextConfig) const {
  if (_tls.sharedTicketKeyStore) {
    // MultiHttpServer worker: one store shared by all the workers.
    return _tls.sharedTicketKeyStore;
  }
  // Keep the keys of the current context while the session ticket settings do not change, so that the tickets it issued
  // still resume after a certificate rotation or another TLS change.
  if (_tls.ctxHolder && currentContextConfig.sessionTickets == _config.tls.sessionTickets &&
      std::ranges::equal(currentContextConfig.sessionTicketKeys(), _config.tls.sessionTicketKeys())) {
    return _tls.ctxHolder->ticketKeyStore();
  }
  return {};
}

void SingleHttpServer::reloadChangedTlsFiles() {
  // Points into the current context: used before it is replaced.
  const std::string_view changedFile = _tls.ctxHolder->changedInputFile();
  if (changedFile.empty()) {
    return;
  }
  try {
    auto newTlsContext = std::make_shared<TlsContext>(_config.tls, ticketKeyStoreForNewTlsContext(_config.tls));
    log::info("TLS context reloaded, '{}' changed on disk", changedFile);
    _tls.ctxHolder = std::move(newTlsContext);
  } catch (const std::exception& ex) {
    // Retried at the next config update (the file still differs from the one the current context was built from).
    log::error("TLS context reload failed after '{}' changed on disk, previous TLS context kept: {}", changedFile,
               ex.what());
  }
}
#endif

void SingleHttpServer::applyPendingUpdates() {
  bool needsClamp = false;

  if (_updates.hasConfig.load(std::memory_order_acquire)) {
    applyPendingConfigUpdates();

    // Reinitialize components dependent on config values.
    _compressionState.selector = EncodingSelector(_config.compression);
    _eventLoop.updatePollTimeoutPolicy(MakePollTimeoutPolicy(_config));
    updateMaintenanceTimer();
    rebuildKeepAliveDeadlines();
    registerBuiltInProbes();
    needsClamp = true;
  }
  if (_updates.hasRouter.load(std::memory_order_acquire)) {
    ApplyPendingUpdates(_updates.lock, _updates.router, _updates.hasRouter, _router, "router");
    needsClamp = true;
  }
  if (_hasCallbacksUpdates.load(std::memory_order_acquire)) {
    ApplyPendingUpdates(_updates.lock, _callbacksUpdates, _hasCallbacksUpdates, _callbacks, "callbacks");
  }

  if (needsClamp) {
    _router.clampConfigs(_config.maxHeaderBytes, _config.maxBodyBytes);
  }

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Process async callbacks posted from background threads
  if (_updates.hasAsyncCallbacks.load(std::memory_order_acquire)) {
    processAsyncCallbacks();
  }
#endif
}

}  // namespace aeronet
