#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <thread>

#include "aeronet/server-config.hpp"
#include "aeronet/server-stats.hpp"
#include "aeronet/server.hpp"
#include "http-method-set.hpp"
#include "http-method.hpp"
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
  using ParserError = HttpServer::ParserError;
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
  //   cfg          - Base ServerConfig applied to each underlying HttpServer. If cfg.port == 0 an
  //                  ephemeral port is chosen by the first server; that resolved port is then
  //                  propagated to all subsequent servers so the entire group listens on the same
  //                  concrete port.
  //   threadCount  - Number of HttpServer instances (and dedicated threads) to launch. Must be >= 1.
  //                  Each instance owns an independent epoll/event loop and shares the listening
  //                  port via SO_REUSEPORT (automatically enabled if threadCount > 1).
  // Behavior:
  //   - Does NOT start the servers; call start() explicitly after registering handlers.
  //   - Validates threadCount and throws invalid_argument if < 1.
  //   - The object is NOT thread-safe; expect single-threaded orchestration.
  // Performance rationale:
  //   - Avoids locks by treating start()/stop()/handler registration as single-threaded control
  //     operations; the hot path remains inside individual HttpServer event loops.
  MultiHttpServer(ServerConfig cfg, uint32_t threadCount);

  // Construct a MultiHttpServer wrapper, with the number of available processors as number of threads (if detection is
  // possible). You can verify how many threads were chosen after construction of this instance thanks to nbThreads()
  // method.
  explicit MultiHttpServer(ServerConfig cfg);

  MultiHttpServer(const MultiHttpServer&) = delete;
  MultiHttpServer(MultiHttpServer&& other) noexcept;
  MultiHttpServer& operator=(const MultiHttpServer&) = delete;
  MultiHttpServer& operator=(MultiHttpServer&& other) noexcept;

  ~MultiHttpServer() { stop(); }

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
  [[nodiscard]] bool isRunning() const { return _running; }

  // port(): The resolved listening port shared by all underlying servers. If an ephemeral port
  //   was requested (cfg.port==0) this becomes available shortly after start() (once the first
  //   server binds). Safe to query post-start; pre-start it is 0 for ephemeral configuration.
  [[nodiscard]] uint16_t port() const { return _resolvedPort; }

  // nbThreads(): Number of underlying HttpServer instances (and threads) configured.
  [[nodiscard]] uint32_t nbThreads() const { return _threadCount; }

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

  ServerConfig _baseConfig;
  uint32_t _threadCount{};
  bool _running{false};
  uint16_t _resolvedPort{};

  std::optional<RequestHandler> _globalHandler;
  vector<PathRegistration> _pathHandlersEmplace;  // store until start()
  ParserErrorCallback _parserErrCb;

  vector<HttpServer> _servers;    // created on start()
  vector<std::jthread> _threads;  // run server.run()
};

}  // namespace aeronet
