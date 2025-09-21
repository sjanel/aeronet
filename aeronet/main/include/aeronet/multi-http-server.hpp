#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>

#include "aeronet/server-config.hpp"
#include "aeronet/server.hpp"
#include "http-method-set.hpp"
#include "http-method.hpp"
#include "string.hpp"
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
    HttpServer::StatsPublic total{};      // summed / max aggregated view
    vector<HttpServer::StatsPublic> per;  // one per underlying server
  };

  MultiHttpServer(ServerConfig cfg, std::size_t threadCount);

  MultiHttpServer(const MultiHttpServer&) = delete;
  MultiHttpServer(MultiHttpServer&& other) noexcept;
  MultiHttpServer& operator=(const MultiHttpServer&) = delete;
  MultiHttpServer& operator=(MultiHttpServer&& other) noexcept;

  ~MultiHttpServer() { stop(); }

  // Register a global handler (mutually exclusive with 'addPathHandler').
  void setHandler(RequestHandler handler);

  // Register a handler for a specific absolute path and a set of allowed HTTP methods.
  void addPathHandler(std::string_view path, const http::MethodSet& methods, const RequestHandler& handler);

  // Register a handler for a specific absolute path and a single allowed HTTP method.
  void addPathHandler(std::string_view path, http::Method method, const RequestHandler& handler);

  // Set a callback to be invoked on HTTP parsing errors.
  void setParserErrorCallback(ParserErrorCallback cb);

  // Start all underlying servers. After this point handler registration is locked.
  void start();

  // Stop and join all server threads.
  void stop();

  [[nodiscard]] bool isRunning() const { return _running; }

  [[nodiscard]] uint16_t port() const { return _resolvedPort; }

  [[nodiscard]] std::size_t size() const { return _threadCount; }

  [[nodiscard]] AggregatedStats stats() const;

 private:
  void ensureNotStarted() const;

  struct PathRegistration {
    string path;
    http::MethodSet methods;
    RequestHandler handler;
  };

  ServerConfig _baseConfig;
  std::size_t _threadCount;
  bool _running{false};
  uint16_t _resolvedPort{};

  std::optional<RequestHandler> _globalHandler;
  vector<PathRegistration> _pathHandlersEmplace;  // store until start()
  ParserErrorCallback _parserErrCb;

  vector<HttpServer> _servers;    // created on start()
  vector<std::jthread> _threads;  // run server.run()
};

}  // namespace aeronet
