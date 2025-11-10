#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/router.hpp"
#include "aeronet/server-stats.hpp"
#include "vector.hpp"

namespace aeronet {

// MultiHttpServer: convenience wrapper that spins up N HttpServer instances
// (each with its own event loop) listening on the same port via SO_REUSEPORT.
// Threads start in start(); they are std::jthread so they auto-join on destruction.
// Handlers must be registered before start(). Post-start registration throws.
// NOTE: This class is intentionally NOT thread-safe. It assumes a single controlling
// thread performs construction, handler registration, start(), stats() calls and stop().
// Dropping the mutex avoids unnecessary synchronization on the hot path of start/stop.
//
// Restart semantics:
//  - MultiHttpServer can be restarted: after stop() you may call start() again. A restart constructs
//    a fresh set of underlying HttpServer instances (HttpServer itself is currently single-shot; its
//    stop() closes the listening socket). Handlers registered prior to the *first* start() are retained;
//    you may also replace the global handler between stops. The same port is reused unless you set
//    _baseConfig.port = 0 (via an external config mutation API in the future) before restarting, in which
//    case a new ephemeral port will be chosen and propagated.
//  - Stats from previous runs are not accumulated across restarts because the underlying servers are rebuilt.
class MultiHttpServer {
 public:
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
  //   threadCount  - Number of HttpServer instances (and dedicated threads) to launch. Must be >= 1.
  //                  Each instance owns an independent epoll/event loop and shares the listening
  //                  port via SO_REUSEPORT (automatically enabled if threadCount > 1).
  // Behavior:
  //   - Does NOT start the servers; call start() explicitly after registering handlers.
  //   - Validates threadCount and throws std::invalid_argument if < 1.
  //   - The object itself is NOT thread-safe; expect single-threaded orchestration.
  // Performance rationale:
  //   - Avoids locks by treating start()/stop()/handler registration as single-threaded control
  //     operations; the hot path remains inside individual HttpServer event loops.
  MultiHttpServer(HttpServerConfig cfg, uint32_t threadCount);

  // Construct a MultiHttpServer wrapper, with the number of available processors as number of threads (if detection is
  // possible). You can verify how many threads were chosen after construction of this instance thanks to nbThreads()
  // method.
  explicit MultiHttpServer(HttpServerConfig cfg);

  // MultiHttpServer is moveable. Rationale:
  //   - Threads capture ONLY raw pointers to stable HttpServer elements stored inside the
  //     _servers std::vector. A move of std::vector transfers ownership of the underlying buffer
  //     without relocating elements (no per-element move), so the addresses remain valid.
  //   - The std::jthread objects are likewise moved; their destructor (join) will occur when the
  //     destination MultiHttpServer is destroyed or stop() is called there.
  //   - The moved-from instance becomes empty (no threads, no servers) and its destructor does
  //     nothing.
  // Constraints:
  //   - Still non-copyable.
  //   - Move assignment will stop() an already-running target before adopting new threads.
  MultiHttpServer(const MultiHttpServer&) = delete;
  MultiHttpServer(MultiHttpServer&& other) noexcept;
  MultiHttpServer& operator=(const MultiHttpServer&) = delete;
  MultiHttpServer& operator=(MultiHttpServer&& other) noexcept;

  ~MultiHttpServer();

  // Returns a reference to the router of this instance.
  // You can modify it as long as the MultiHttpServer is not started.
  // Prerequisites: 'empty()' should be 'false'
  Router& router();

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

  // start():
  //   Launches the configured number of HttpServer instances, each on its own std::jthread.
  //   Enables SO_REUSEPORT automatically when threadCount > 1 (overrides cfg.reusePort=false).
  //   For ephemeral ports (cfg.port==0): waits (busy sleep up to ~200ms) for the first server to
  //   resolve a concrete port, then propagates that port to subsequent instances so the entire
  //   group listens on a single shared port.
  // Error handling:
  //   - Throws std::logic_error if called more than once.
  //   - Exceptions during individual HttpServer::run() are logged; that thread exits but others
  //     continue (future improvement could surface an aggregated failure signal).
  // Post-conditions:
  //   - isRunning() returns true if startup sequence completed.
  //   - Handler registration becomes immutable after this call.
  void start();

  // stop():
  //   Signals all underlying servers to stop, then joins their threads (via std::jthread RAII
  //   on scope exit of internal moves). Safe to call multiple times; subsequent calls are no-ops.
  //   Blocks until all servers have exited their event loops. Ensures HttpServer objects outlive
  //   the joining of their threads (ordering guaranteed by move+scope pattern in implementation).
  void stop() noexcept;

  // beginDrain(): forward graceful drain to every underlying HttpServer.
  void beginDrain(std::chrono::milliseconds maxWait = std::chrono::milliseconds{0}) noexcept;

  // Checks if this instance is empty (ie: it contains no server instances and should not be configured).
  [[nodiscard]] bool empty() const noexcept { return _servers.empty(); }

  // isRunning(): true after successful start() and before stop() completion.
  //   Reflects the high-level lifecycle, not the liveness of each individual thread (a thread
  //   may have terminated due to an exception while isRunning() is still true). Use stats() or
  //   external health checks for deeper diagnostics.
  [[nodiscard]] bool isRunning() const { return !_threads.empty(); }

  [[nodiscard]] bool isDraining() const;

  // port(): The resolved listening port shared by all underlying servers.
  // Returns 0 if the instance is empty (holding no servers)
  [[nodiscard]] uint16_t port() const { return empty() ? 0 : _servers[0].port(); }

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

 private:
  void canSetCallbacks() const;

  // single-writer (controller thread), multi-reader (worker threads)
  // It is useful to avoid freezes when stop() before the server thread has entered the main loop after start.
  std::unique_ptr<std::atomic<bool>> _stopRequested;

  // IMPORTANT LIFETIME NOTE:
  // Each server thread captures a raw pointer to its corresponding HttpServer element stored in _servers.
  // We must therefore ensure that the pointed-to HttpServer objects remain alive until after the jthreads join.
  // Destruction order is reverse of declaration order, so declare 'servers' BEFORE 'threads'.
  vector<HttpServer> _servers;    // created on start()
  vector<std::jthread> _threads;  // run server.run()
};

}  // namespace aeronet
