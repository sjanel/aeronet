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
#include "aeronet/internal/server-context.hpp"
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
  return {[](void* pServer, std::function<void(Router&)> updater) {
            auto completion = std::make_shared<std::promise<std::exception_ptr>>();
            auto future = completion->get_future();
            static_cast<SingleHttpServer*>(pServer)->submitRouterUpdate(std::move(updater), std::move(completion));
            if (auto ex = future.get()) {
              std::rethrow_exception(ex);
            }
          },
          this};
}

void SingleHttpServer::setParserErrorCallback(ParserErrorCallback cb) {
  submitCallbacksUpdate(
      [cb = std::move(cb)](internal::ServerCallbacks& callbacks) mutable { callbacks.parserErr = std::move(cb); });
}

void SingleHttpServer::setMetricsCallback(MetricsCallback cb) {
  submitCallbacksUpdate(
      [cb = std::move(cb)](internal::ServerCallbacks& callbacks) mutable { callbacks.metrics = std::move(cb); });
}

#ifdef AERONET_ENABLE_OPENSSL
void SingleHttpServer::setTlsHandshakeCallback(TlsHandshakeCallback cb) {
  submitCallbacksUpdate(
      [cb = std::move(cb)](internal::ServerCallbacks& callbacks) mutable { callbacks.tlsHandshake = std::move(cb); });
}
#endif

void SingleHttpServer::setExpectationHandler(ExpectationHandler handler) {
  submitCallbacksUpdate([handler = std::move(handler)](internal::ServerCallbacks& callbacks) mutable {
    callbacks.expectation = std::move(handler);
  });
}

void SingleHttpServer::setMiddlewareMetricsCallback(MiddlewareMetricsCallback cb) {
  submitCallbacksUpdate([cb = std::move(cb)](internal::ServerCallbacks& callbacks) mutable {
    callbacks.middlewareMetrics = std::move(cb);
  });
}

void SingleHttpServer::submitCallbacksUpdate(std::function<void(internal::ServerCallbacks&)> updater) {
  if (_ctx.lifecycle.isEventLoopThread()) {
    // From a handler or a callback: the event loop is the thread using the callbacks.
    updater(_ctx.callbacks);
    return;
  }

  // While running, the change is applied by the event loop, and this call waits for it: the new callback is then used
  // for everything happening after this call (the event loop may otherwise still process, in its current iteration, a
  // connection initiated after it).
  auto applied = std::make_shared<std::atomic<bool>>(false);
  {
    std::scoped_lock lock(_ctx.updates.lock);
    if (_ctx.lifecycle.isIdle()) {
      applyQueuedCallbacksUpdates();
      updater(_ctx.callbacks);
      return;
    }
    _callbacksUpdates.emplace_back([updater = std::move(updater), applied](internal::ServerCallbacks& callbacks) {
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
  _ctx.lifecycle.wakeupFd.send();

  while (!applied->load(std::memory_order_acquire)) {
    {
      std::scoped_lock lock(_ctx.updates.lock);
      if (_ctx.lifecycle.isIdle()) {
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
    pendingUpdater(_ctx.callbacks);
  }
  _callbacksUpdates.clear();
  _hasCallbacksUpdates.store(false, std::memory_order_relaxed);
}

void SingleHttpServer::postConfigUpdate(std::function<void(HttpServerConfig&)> updater) {
  {
    std::scoped_lock lock(_ctx.updates.lock);
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

    _ctx.updates.config.emplace_back(WrappedUpdater{std::move(updater)});
    _ctx.updates.hasConfig.store(true, std::memory_order_release);
  }
  _ctx.lifecycle.wakeupFd.send();
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
    std::scoped_lock lock(_ctx.updates.lock);
    // beginStartup() takes this same lock before publishing Starting. A direct update
    // therefore completes before startup begins; every later update is queued for the
    // event-loop thread and clamped there.
    if (_ctx.lifecycle.isIdle()) {
      wrappedUpdater(_ctx.router);
      return;
    }
    _ctx.updates.router.emplace_back(std::move(wrappedUpdater));
    _ctx.updates.hasRouter.store(true, std::memory_order_release);
  }
  _ctx.lifecycle.wakeupFd.send();
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
  for (auto& updater : TakePendingUpdates(_ctx.updates.lock, _ctx.updates.config, _ctx.updates.hasConfig)) {
    try {
      // Each update is applied atomically: if the updater, the validation or the construction of the objects built from
      // the new configuration (TLS context, access log writer) throws, the previous configuration is restored.
      HttpServerConfig previousConfig(_ctx.config);
      try {
        updater(_ctx.config);
        _ctx.config.validate();

#ifdef AERONET_ENABLE_OPENSSL
        // Existing connections keep the context they were created from (ConnectionState::tlsContextKeepAlive).
        std::shared_ptr<TlsContext> newTlsContext;
        const bool tlsChanged = _ctx.config.tls != previousConfig.tls;
        if (tlsChanged && _ctx.config.tls.enabled) {
          newTlsContext = _ctx.tls.makeContext(previousConfig.tls);
        }
#endif
        AccessLogWriter newAccessLog;
        const bool accessLogChanged = _ctx.config.accessLog != previousConfig.accessLog;
        if (accessLogChanged) {
          newAccessLog = AccessLogWriter(_ctx.config.accessLog);
        }

        // Commit - nothing can throw from here.
#ifdef AERONET_ENABLE_OPENSSL
        if (tlsChanged) {
          _ctx.tls.setContext(std::move(newTlsContext));
          tlsContextRebuilt = true;
        }
#endif
        if (accessLogChanged) {
          // The previous writer, swapped into newAccessLog, flushes its buffered lines when destroyed.
          std::swap(_ctx.accessLog, newAccessLog);
        }
      } catch (...) {
        // Swap rather than assign: the rejected configuration is then destroyed with previousConfig, which scrubs its
        // TLS secrets (a move assignment would free their buffers without scrubbing them).
        std::swap(_ctx.config, previousConfig);
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
  if (!tlsContextRebuilt && _ctx.tls.hasContext()) {
    _ctx.tls.reloadChangedFiles();
  }
#endif
}

void SingleHttpServer::applyPendingUpdates() {
  bool needsClamp = false;

  if (_ctx.updates.hasConfig.load(std::memory_order_acquire)) {
    applyPendingConfigUpdates();

    // Reinitialize components dependent on config values.
    _ctx.compressionState.selector = EncodingSelector(_ctx.config.compression);
    _ctx.eventLoop.updatePollTimeoutPolicy(internal::MakePollTimeoutPolicy(_ctx.config));
    updateMaintenanceTimer();
    _ctx.maintenance.rebuildKeepAliveDeadlines();
    registerBuiltInProbes();
    needsClamp = true;
  }
  if (_ctx.updates.hasRouter.load(std::memory_order_acquire)) {
    ApplyPendingUpdates(_ctx.updates.lock, _ctx.updates.router, _ctx.updates.hasRouter, _ctx.router, "router");
    needsClamp = true;
  }
  if (_hasCallbacksUpdates.load(std::memory_order_acquire)) {
    ApplyPendingUpdates(_ctx.updates.lock, _callbacksUpdates, _hasCallbacksUpdates, _ctx.callbacks, "callbacks");
  }

  if (needsClamp) {
    _ctx.router.clampConfigs(_ctx.config.maxHeaderBytes, _ctx.config.maxBodyBytes);
  }

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
  // Process async callbacks posted from background threads
  if (_ctx.updates.hasAsyncCallbacks.load(std::memory_order_acquire)) {
    processAsyncCallbacks();
  }
#endif
}

}  // namespace aeronet
