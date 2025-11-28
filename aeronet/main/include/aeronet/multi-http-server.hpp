#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>

#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/router-update-proxy.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-lifecycle-tracker.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

// MultiHttpServer: convenience wrapper that spins up N HttpServer instances
// (each with its own event loop) listening on the same port via SO_REUSEPORT.
//
// Restart semantics:
//  - MultiHttpServer can be restarted: after stop() you may call start() again. A restart constructs
//    a fresh set of underlying HttpServer instances (HttpServer itself is currently single-shot; its
//    stop() closes the listening socket). Handlers registered prior to the *first* start() are retained;
//    you may also replace the global handler between stops. The same port is reused.
//  - Stats from previous runs are not accumulated across restarts because the underlying servers are rebuilt.
class MultiHttpServer {
 public:
  // AsyncHandle: RAII wrapper for non-blocking multi-server execution
  // ------------------------------------------------------------------
  // Returned by start() methods to manage the background threads running the MultiHttpServer instances.
  // Provides lifetime management (RAII join on destruction) and error propagation from background threads.
  //
  // Typical usage:
  //   MultiHttpServer multi(cfg, router, 4);
  //   auto handle = multi.start();  // non-blocking, returns immediately
  //   // ... do work while servers run in background ...
  //   handle.stop();  // or let handle destructor auto-stop
  //   handle.rethrowIfError();  // check for exceptions from any event loop
  struct HandleCompletion;

  class AsyncHandle {
   public:
    AsyncHandle(const AsyncHandle&) = delete;
    AsyncHandle& operator=(const AsyncHandle&) = delete;

    AsyncHandle(AsyncHandle&& other) noexcept;
    AsyncHandle& operator=(AsyncHandle&& other) noexcept;

    // Destructor automatically stops and joins all background threads (RAII)
    ~AsyncHandle();

    // Stop all background event loops and join threads (blocking).
    // Safe to call multiple times; subsequent calls are no-ops.
    void stop() noexcept;

    // Rethrow the first exception that occurred in any background event loop.
    // Call after stop() to check for errors.
    void rethrowIfError();

    // Check if any background thread is still active (not yet joined).
    [[nodiscard]] bool started() const noexcept;

   private:
    friend class MultiHttpServer;

    AsyncHandle(vector<HttpServer::AsyncHandle> serverHandles, std::shared_ptr<std::atomic<bool>> stopRequested,
                std::shared_ptr<std::function<void()>> onStop, std::shared_ptr<HandleCompletion> completion,
                std::shared_ptr<ServerLifecycleTracker> lifecycleTracker, std::shared_ptr<void> stopTokenBinding);

    vector<HttpServer::AsyncHandle> _serverHandles;
    std::shared_ptr<std::atomic<bool>> _stopRequested;  // shared with server logic
    std::shared_ptr<std::function<void()>> _onStop;
    std::shared_ptr<HandleCompletion> _completion;
    std::shared_ptr<ServerLifecycleTracker> _lifecycleTracker;
    std::shared_ptr<void> _stopTokenBinding;
    std::atomic<bool> _stopCalled{false};

    void notifyCompletion() noexcept;
  };

  // Construct a MultiHttpServer that does nothing.
  // Useful only to make it default constructible for temporary purposes (for instance to move assign to it later on),
  // but do not attempt to use a default constructed server, it will not bind to any socket.
  MultiHttpServer() noexcept = default;

  // Construct a MultiHttpServer wrapper.
  // Parameters:
  //   cfg          - Base HttpServerConfig applied to each underlying HttpServer. If cfg.port == 0 an
  //                  ephemeral port is chosen by the first server; that resolved port is then
  //                  propagated to all subsequent servers so the entire group listens on the same
  //                  concrete port.
  //   threadCount  - Number of HttpServer instances (and dedicated threads) to launch.
  //                  if 0, attempt to guess from the underlying hardware concurrency.
  //                  Each instance owns an independent epoll/event loop and shares the listening
  //                  port via SO_REUSEPORT (automatically enabled if threadCount > 1).
  // Behavior:
  //   - Does NOT start the servers, you need to call start() or run() family of methods to launch them.
  //   - The object itself is NOT thread-safe; expect single-threaded orchestration.
  MultiHttpServer(HttpServerConfig cfg, Router router, uint32_t threadCount);

  // Variant of MultiHttpServer(HttpServerConfig, Router, uint32_t) with a default constructed
  // Router.
  MultiHttpServer(HttpServerConfig cfg, uint32_t threadCount)
      : MultiHttpServer(std::move(cfg), Router(), threadCount) {}

  // Construct a MultiHttpServer wrapper, with the number of available processors as number of threads (if detection is
  // possible). You can verify how many threads were chosen after construction of this instance thanks to nbThreads()
  // method.
  MultiHttpServer(HttpServerConfig cfg, Router router) : MultiHttpServer(std::move(cfg), std::move(router), 0) {}

  // Variant of MultiHttpServer(HttpServerConfig, Router) with a default constructed router.
  explicit MultiHttpServer(HttpServerConfig cfg) : MultiHttpServer(std::move(cfg), Router()) {}

  // MultiHttpServer is copyable as long as the source is fully stopped. Copies rebuild fresh HttpServer instances
  // with the same configuration but do not share lifecycle trackers or stop tokens with the source.
  MultiHttpServer(const MultiHttpServer&);
  MultiHttpServer(MultiHttpServer&& other) noexcept;
  MultiHttpServer& operator=(const MultiHttpServer&);
  MultiHttpServer& operator=(MultiHttpServer&& other) noexcept;

  ~MultiHttpServer();

  // Obtain a proxy enabling fluent router updates across all underlying servers.
  RouterUpdateProxy router();

  // setParserErrorCallback:
  //   Installs a callback invoked by each underlying HttpServer when a parser error occurs
  //   (see HttpServer::ParserError). Used for centralized metrics or logging.
  // Constraints:
  //   - Must be set before start(); post-start modification throws.
  // Lifetime:
  //   - The callback is copied into each server at start() time.
  // Clearing:
  //   - Pass an empty std::function to clear prior to start().
  void setParserErrorCallback(HttpServer::ParserErrorCallback cb);

  // Sets a callback invoked after completing each request on every underlying server.
  // See HttpServer::setMetricsCallback for semantics. The callback is copied into each
  // HttpServer instance at start() time. Must be set before start().
  void setMetricsCallback(HttpServer::MetricsCallback cb);

  // Install a custom expectation handler on all underlying servers. Copied into each
  // HttpServer at start() time. Must be set before start().
  void setExpectationHandler(HttpServer::ExpectationHandler handler);

  // Install a middleware metrics callback on all underlying servers. Copied into each
  // HttpServer at start() time. Must be set before start().
  void setMiddlewareMetricsCallback(HttpServer::MiddlewareMetricsCallback cb);

  // run():
  //   Blocking variant of start(). Launches the configured number of HttpServer instances and
  //   blocks the calling thread until all servers complete (via stop() or graceful drain completion).
  //   Functionally equivalent to: start(); <wait for all threads to complete>; stop();
  // Error handling:
  //   - Throws std::logic_error if called more than once.
  //   - Exceptions during individual HttpServer event loops are logged; that thread exits but others
  //     continue until all have completed.
  // Post-conditions:
  //   - When run() returns, all server threads have exited and the MultiHttpServer is stopped.
  //   - The instance can be restarted by calling start() or run() again (handlers are retained).
  // Use case:
  //   - Primary use case for simple applications where the main thread should block serving requests
  //     until shutdown is triggered (e.g., via signal handler calling beginDrain() or stop()).
  void run();

  // runUntil():
  //   Blocking variant that keeps serving until the supplied predicate returns true or stop() is invoked.
  //   The predicate is evaluated from each worker thread between event loop iterations, mirroring
  //   HttpServer::runUntil semantics. The predicate must therefore be thread-safe. When the predicate
  //   fires, all servers are requested to stop and the method waits until every worker thread exits.
  void runUntil(const std::function<bool()>& predicate);

  // stop():
  //   Signals all underlying servers to stop, then joins their threads (via std::jthread RAII
  //   on scope exit of internal moves). Safe to call multiple times; subsequent calls are no-ops.
  //   Blocks until all servers have exited their event loops. Ensures HttpServer objects outlive
  //   the joining of their threads (ordering guaranteed by move+scope pattern in implementation).
  void stop() noexcept;

  // start():
  //   Launches the configured number of HttpServer instances, each on its own background thread.
  //   The server manages the thread lifetime internally and will automatically stop and join all
  //   threads when the server is destroyed or stop() is called. This is the simple, recommended
  //   API for most use cases.
  // Error handling:
  //   - Throws std::logic_error if called while already running.
  //   - Exceptions during individual HttpServer::run() are captured and can be rethrown via stop() cleanup.
  // Post-conditions:
  //   - Returns immediately (non-blocking); servers run in background threads.
  //   - Handler registration becomes immutable after this call.
  void start();

  // startDetached():
  //   Like start(), but returns an AsyncHandle for explicit lifetime management.
  //   Use this when you need fine-grained control (checking if started, rethrowIfError, etc).
  //   The AsyncHandle will automatically stop and join all threads on destruction (RAII).
  // Error handling:
  //   - Throws std::logic_error if called while already running.
  //   - Exceptions during individual HttpServer::run() are captured and can be rethrown via handle.rethrowIfError().
  // Post-conditions:
  //   - Returns immediately with an AsyncHandle; servers run in background threads.
  //   - Handler registration becomes immutable after this call.
  [[nodiscard]] AsyncHandle startDetached();

  // startDetachedAndStopWhen():
  //   Like startDetached(), but also terminates the event loops when the provided predicate returns true.
  //   Useful for coordinating shutdown with external signals. Predicate is evaluated from each worker thread.
  [[nodiscard]] AsyncHandle startDetachedAndStopWhen(std::function<bool()> predicate);

  // startDetachedWithStopToken():
  //   Launch the event loops and stop them when either the returned AsyncHandle is stopped or the provided
  //   std::stop_token is triggered. Handy for integrating with cooperative cancellation infrastructure.
  [[nodiscard]] AsyncHandle startDetachedWithStopToken(const std::stop_token& token);

  // beginDrain(): forward graceful drain to every underlying HttpServer.
  void beginDrain(std::chrono::milliseconds maxWait = std::chrono::milliseconds{0}) noexcept;

  // Checks if this instance is empty (ie: it contains no server instances and should not be configured).
  // This can only happen in two cases:
  //   - Default constructed MultiHttpServer.
  //   - Moved-from MultiHttpServer.
  [[nodiscard]] bool empty() const noexcept { return _servers.empty(); }

  // isRunning(): true after successful start() and before stop() completion.
  //   Reflects the high-level lifecycle, not the liveness of each individual thread (a thread
  //   may have terminated due to an exception while isRunning() is still true). Use stats() or
  //   external health checks for deeper diagnostics.
  [[nodiscard]] bool isRunning() const { return _internalHandle.has_value() || !_lastHandleStopFn.expired(); }

  // isDraining(): true if all underlying servers are currently draining.
  [[nodiscard]] bool isDraining() const;

  // Access the telemetry context for custom tracing/spans.
  // The returned reference is valid for the lifetime of the MultiHttpServer instance,
  // but is invalidated if the server is moved.
  // Precondition: empty() is false, otherwise undefined behavior.
  [[nodiscard]] const tracing::TelemetryContext& telemetryContext() const noexcept {
    return _servers[0].telemetryContext();
  }

  // port(): The resolved listening port shared by all underlying servers.
  // If the port was ephemeral (0) at construction time, this returns the concrete port chosen by
  // the first server.
  // Precondition: empty() is false, otherwise undefined behavior.
  [[nodiscard]] uint16_t port() const { return _servers[0].port(); }

  // nbThreads(): Number of underlying HttpServer instances (and threads) configured.
  [[nodiscard]] uint32_t nbThreads() const { return _servers.capacity(); }

  struct AggregatedStats {
    // JSON array of per-instance objects
    [[nodiscard]] std::string json_str() const;

    // Aggregated view across all underlying servers.
    ServerStats total{};
    // Per-instance snapshots
    vector<ServerStats> per;
  };

  // stats():
  //   Collects statistics from each underlying HttpServer and returns both per-instance and
  //   aggregated totals. Costs O(N) in number of servers and should be used sparingly in hot
  //   telemetry paths. Thread-safe for read-only access under assumption that start()/stop()
  //   are not racing with this call (class not fully synchronized).
  [[nodiscard]] AggregatedStats stats() const;

  // Post a configuration update to be applied safely to all underlying servers.
  // See HttpServer::postConfigUpdate for semantics.
  void postConfigUpdate(const std::function<void(HttpServerConfig&)>& updater);

  // Schedule a router update applied to every underlying HttpServer on the event loop threads.
  void postRouterUpdate(std::function<void(Router&)> updater);

 private:
  void canSetCallbacks() const;

  void ensureNextServersBuilt();

  [[nodiscard]] vector<HttpServer*> collectServerPointers();

  void runBlocking(std::function<bool()> predicate, std::string_view modeLabel);

  [[nodiscard]] AsyncHandle startDetachedInternal(std::function<bool()> extraStopCondition,
                                                  const std::stop_token& externalStopToken);

  // single-writer (controller thread), multi-reader (worker threads)
  // It is useful to avoid freezes when stop() before the server thread has entered the main loop after start.
  // Shared ownership allows both MultiHttpServer and AsyncHandle to control stopping.
  std::shared_ptr<std::atomic<bool>> _stopRequested{std::make_shared<std::atomic<bool>>(false)};
  std::shared_ptr<ServerLifecycleTracker> _lifecycleTracker{std::make_shared<ServerLifecycleTracker>()};

  // IMPORTANT LIFETIME NOTE:
  // Each server thread captures a raw pointer to its corresponding HttpServer element stored in _servers.
  // We must therefore ensure that the pointed-to HttpServer objects remain alive until after the jthreads join.
  // Destruction order is reverse of declaration order, so declare 'servers' BEFORE 'threads'.
  vector<HttpServer> _servers;  // created on start()

  // Internal handle for simple start() API - managed by the server itself.
  // When start() is called, the handle is stored here and the server takes ownership.
  // When startDetached() is called, the handle is returned to the caller.
  std::optional<AsyncHandle> _internalHandle;
  std::weak_ptr<std::function<void()>> _lastHandleStopFn;
  std::weak_ptr<HandleCompletion> _lastHandleCompletion;
  std::shared_ptr<std::atomic<bool>> _serversAlive{std::make_shared<std::atomic<bool>>(true)};
};

}  // namespace aeronet
