#pragma once

#include <exception>
#include <thread>
#include <utility>

#include "aeronet/http-server.hpp"

namespace aeronet {

// Owns a single HttpServer instance and runs its event loop in a dedicated std::jthread.
// Simplifies lifetime: destroying AsyncHttpServer always joins the thread before destroying
// the owned HttpServer. Provides server() accessor for configuration prior to start().
//
// Blocking vs Non-Blocking Summary:
//   HttpServer::run()/runUntil()  -> blocking
//   AsyncHttpServer::start()      -> non-blocking (1 background thread)
//   MultiHttpServer::start()      -> non-blocking (N background threads)
//
// Basic usage:
//   AsyncHttpServer async(HttpServer(HttpServerConfig{}.withPort(0)));
//   async.server().setHandler(...);
//   async.start();
//   // ... work ...
//   async.requestStop(); async.stopAndJoin();
//
// Move-in pattern (already configured server):
//   HttpServer s(cfg);
//   s.setHandler(...);
//   AsyncHttpServer async(std::move(s));
//   async.start();
//
// Predicate:
//   async.startUntil([&]{ return done.load(); });
//
// Thread-safety: same as HttpServer (not internally synchronized). Configure before start or
// coordinate externally if changing handlers after start.
class AsyncHttpServer {
 public:
  // Take ownership (by value) of a configured HttpServer.
  explicit AsyncHttpServer(HttpServer server) noexcept;

  AsyncHttpServer(const AsyncHttpServer&) = delete;
  AsyncHttpServer(AsyncHttpServer&& other) noexcept;

  AsyncHttpServer& operator=(const AsyncHttpServer&) = delete;
  AsyncHttpServer& operator=(AsyncHttpServer&& other) noexcept;

  ~AsyncHttpServer();

  [[nodiscard]] bool started() const noexcept { return _thread.joinable(); }

  HttpServer& server() noexcept { return _server; }
  [[nodiscard]] const HttpServer& server() const noexcept { return _server; }

  void start();

  template <class Predicate>
  void startUntil(Predicate pred) {
    ensureStartable();
    _thread =
        std::jthread([this, pred = std::move(pred)](std::stop_token st) mutable { runLoopWithPredicate(st, pred); });
  }

  void requestStop() noexcept;

  void stopAndJoin() noexcept;

  void rethrowIfError();

  template <class... Args>
  static AsyncHttpServer makeFromConfig(Args&&... args) {
    return AsyncHttpServer(HttpServer(std::forward<Args>(args)...));
  }

 private:
  void ensureStartable();

  void runLoopNoPredicate(const std::stop_token& st) noexcept;

  template <class Predicate>
  void runLoopWithPredicate(const std::stop_token& st, Predicate& pred) noexcept {
    try {
      _server.runUntil([&]() { return st.stop_requested() || pred(); });
    } catch (...) {
      _error = std::current_exception();
    }
  }

  HttpServer _server;
  std::jthread _thread;       // background loop thread
  std::exception_ptr _error;  // captured exception from loop
};

// Convenience free function
inline AsyncHttpServer runAsync(HttpServer server) {
  AsyncHttpServer async(std::move(server));
  async.start();
  return async;
}

}  // namespace aeronet
