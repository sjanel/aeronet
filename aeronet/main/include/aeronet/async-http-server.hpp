#pragma once

#include <exception>
#include <thread>

#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/router.hpp"

namespace aeronet {

// Owns a single HttpServer instance and runs its event loop in a dedicated std::jthread.
// Simplifies lifetime: destroying AsyncHttpServer always joins the thread before destroying
// the owned HttpServer.
// AsyncHttpServer is restartable.
//
// Blocking vs Non-Blocking Summary:
//   HttpServer::run()/runUntil()  -> blocking
//   AsyncHttpServer::start()      -> non-blocking (1 background thread)
//   MultiHttpServer::start()      -> non-blocking (N background threads)
//
// Basic usage:
//   AsyncHttpServer async(HttpServerConfig{}.withPort(0));
//   async.router().setDefault(...);
//   async.start();
//   // ... work ...
//   async.stop();
//
// Predicate:
//   async.startUntil([&]{ return done.load(); });
//
// Thread-safety: same as HttpServer (not internally synchronized). Configure before start or
// coordinate externally if changing handlers after start.
class AsyncHttpServer {
 public:
  // Construct a AsyncHttpServer that does nothing.
  // Useful only to make it default constructible for temporary purposes (for instance to move assign to it later on),
  // but do not attempt to use a default constructed server, it will not bind to any socket.
  AsyncHttpServer() noexcept = default;

  // Creates a new AsyncHttpServer from given config.
  explicit AsyncHttpServer(HttpServerConfig httpServerConfig);

  // Creates a new AsyncHttpServer from a HttpServer (already configured, or not).
  explicit AsyncHttpServer(HttpServer server);

  // Creates a new AsyncHttpServer from given config and router.
  AsyncHttpServer(HttpServerConfig httpServerConfig, Router router);

  AsyncHttpServer(const AsyncHttpServer&) = delete;
  AsyncHttpServer(AsyncHttpServer&& other) noexcept;

  AsyncHttpServer& operator=(const AsyncHttpServer&) = delete;
  AsyncHttpServer& operator=(AsyncHttpServer&& other) noexcept;

  ~AsyncHttpServer();

  [[nodiscard]] bool started() const noexcept { return _thread.joinable(); }

  // Get a reference to the router object of this instance.
  // You may use this to query or modify path handlers after initial configuration.
  Router& router() noexcept { return _server.router(); }

  void setParserErrorCallback(HttpServer::ParserErrorCallback cb);
  void setMetricsCallback(HttpServer::MetricsCallback cb);

  // Server port. You can call this method directly after construction, ephemeral ports will be resolved.
  [[nodiscard]] uint16_t port() const noexcept { return _server.port(); }

  // Start the main loop in another thread (different from the caller), so this call is non-blocking.
  // Call stop() to ask for termination of the server loop (ideally from the same thread that called start()).
  // Exceptions from the server are stored internally and you can use `rethrowIfError` after stop to rethrow exception.
  void start();

  // Same as start(), but with an additional predicate, that returns 'true' to indicate stop requested.
  void startUntil(std::function<bool()> predicate);

  // Stops the main loop, should be called after 'start()' or 'startUntil()'.
  // This call is blocking for current thread, until the underlying server is stopped.
  // After stop(), it is possible to call start() again.
  void stop() noexcept;

  // If an exception has been thrown during the server loop, rethrow the exception in main process.
  void rethrowIfError();

 private:
  void ensureStartable();

  HttpServer _server;
  std::jthread _thread;       // background loop thread
  std::exception_ptr _error;  // captured exception from loop
};

}  // namespace aeronet
