#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#include "aeronet/http-method-set.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
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
class MultiHttpServer {
 public:
  using RequestHandler = HttpServer::RequestHandler;
  using ParserErrorCallback = HttpServer::ParserErrorCallback;

  struct AggregatedStats {
    // Aggregated view across all underlying servers.
    ServerStats total{};
    // Per-instance snapshots
    vector<ServerStats> per;
    // JSON array of per-instance objects
    [[nodiscard]] std::string json_str() const;
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
  //   threadCount  - Number of HttpServer instances (and dedicated threads) to launch. Must be >= 1.
  //                  Each instance owns an independent epoll/event loop and shares the listening
  //                  port via SO_REUSEPORT (automatically enabled if threadCount > 1).
  // Behavior:
  //   - Does NOT start the servers; call start() explicitly after registering handlers.
  //   - Validates threadCount and throws invalid_argument if < 1.
  //   - The object itself is NOT thread-safe; expect single-threaded orchestration.
  // Performance rationale:
  //   - Avoids locks by treating start()/stop()/handler registration as single-threaded control
  //     operations; the hot path remains inside individual HttpServer event loops.
  MultiHttpServer(HttpServerConfig cfg, uint32_t threadCount);

  // Construct a MultiHttpServer wrapper, with the number of available processors as number of threads (if detection is
  // possible). You can verify how many threads were chosen after construction of this instance thanks to nbThreads()
  // method.
  explicit MultiHttpServer(HttpServerConfig cfg);

  // Move semantics:
  // ---------------
  // We now ALLOW moving a running MultiHttpServer. Rationale:
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
  MultiHttpServer(MultiHttpServer&& other) noexcept;  // NOLINT(modernize-use-noexcept)
  MultiHttpServer& operator=(const MultiHttpServer&) = delete;
  MultiHttpServer& operator=(MultiHttpServer&& other) noexcept;  // NOLINT(modernize-use-noexcept)

  ~MultiHttpServer();

  // setHandler:
  //   Registers a single global handler applied to all successfully parsed requests on every
  //   underlying HttpServer instance. Mutually exclusive with addPathHandler registrations.
  // Constraints:
  //   - Must be invoked before start().
  //   - Throws std::logic_error if path handlers have already been added.
  // Threading:
  //   - Not thread-safe; call from the controlling thread only.
  // Replacement:
  //   - May be called multiple times pre-start; the last handler wins.
  void setHandler(RequestHandler handler);

  // addPathHandler (multi-method):
  //   Registers a handler for a given absolute path and a fixed set of allowed HTTP methods.
  // Behavior:
  //   - Paths are matched exactly (no globbing / parameter extraction in current phase).
  //   - The supplied MethodSet is converted to an internal method bitmask for dispatch speed.
  // Constraints:
  //   - Must be called before start().
  //   - Incompatible with a previously set global handler (logic_error if violated).
  // Multiple registrations:
  //   - Re-registering the same path overwrites the previous mapping for the specified methods.
  void addPathHandler(std::string path, const http::MethodSet& methods, const RequestHandler& handler);

  // addPathHandler (single method convenience):
  //   Shorthand for registering exactly one allowed method for a path. Internally builds a
  //   temporary MethodSet then delegates to the multi-method overload. Same constraints apply.
  void addPathHandler(std::string path, http::Method method, const RequestHandler& handler);

  // setParserErrorCallback:
  //   Installs a callback invoked by each underlying HttpServer when a parser error occurs
  //   (see HttpServer::ParserError). Used for centralized metrics or logging.
  // Constraints:
  //   - Must be set before start(); post-start modification throws.
  // Lifetime:
  //   - The callback is copied into each server at start() time.
  // Clearing:
  //   - Pass an empty std::function to clear prior to start().
  void setParserErrorCallback(ParserErrorCallback cb);

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
  void stop();

  // isRunning(): true after successful start() and before stop() completion.
  //   Reflects the high-level lifecycle, not the liveness of each individual thread (a thread
  //   may have terminated due to an exception while isRunning() is still true). Use stats() or
  //   external health checks for deeper diagnostics.
  [[nodiscard]] bool isRunning() const { return !_threads.empty(); }

  // port(): The resolved listening port shared by all underlying servers. If an ephemeral port
  //   was requested (cfg.port==0) this becomes available directly after construction.
  [[nodiscard]] uint16_t port() const { return _baseConfig.port; }

  // nbThreads(): Number of underlying HttpServer instances (and threads) configured.
  [[nodiscard]] uint32_t nbThreads() const { return _servers.size(); }

  // stats():
  //   Collects statistics from each underlying HttpServer and returns both per-instance and
  //   aggregated totals. Costs O(N) in number of servers and should be used sparingly in hot
  //   telemetry paths. Thread-safe for read-only access under assumption that start()/stop()
  //   are not racing with this call (class not fully synchronized).
  [[nodiscard]] AggregatedStats stats() const;

 private:
  void ensureNotStarted() const;

  struct PathRegistration {
    std::string path;
    http::MethodSet methods;
    RequestHandler handler;
  };

  HttpServerConfig _baseConfig;

  std::optional<RequestHandler> _globalHandler;
  vector<PathRegistration> _pathHandlersEmplace;
  ParserErrorCallback _parserErrCb;

  // IMPORTANT LIFETIME NOTE:
  // Each server thread captures a raw pointer to its corresponding HttpServer element stored in _servers.
  // We must therefore ensure that the pointed-to HttpServer objects remain alive until after the jthreads join.
  // std::jthread joins in its destructor, so we arrange destruction order such that:
  //   1. Local 'threads' (moved from _threads) is destroyed FIRST (joins threads) while servers are still alive.
  //   2. Local 'servers' (moved from _servers) is destroyed AFTER 'threads', releasing HttpServer objects safely.
  // Destruction order is reverse of declaration order, so declare 'servers' BEFORE 'threads'.
  vector<HttpServer> _servers;    // created on start()
  vector<std::jthread> _threads;  // run server.run()
};

}  // namespace aeronet
