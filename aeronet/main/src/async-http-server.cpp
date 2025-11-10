#include "aeronet/async-http-server.hpp"

#include <chrono>
#include <exception>
#include <functional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

#include "aeronet/http-server-config.hpp"
#include "aeronet/http-server.hpp"
#include "aeronet/router.hpp"

namespace aeronet {

AsyncHttpServer::AsyncHttpServer(HttpServerConfig httpServerConfig) : _server(std::move(httpServerConfig)) {}

AsyncHttpServer::AsyncHttpServer(HttpServerConfig httpServerConfig, Router router)
    : _server(std::move(httpServerConfig), std::move(router)) {}

AsyncHttpServer::AsyncHttpServer(HttpServer server) : _server(std::move(server)) {}

AsyncHttpServer::AsyncHttpServer(AsyncHttpServer&& other) noexcept
    : _server(std::move(other._server)), _thread(std::move(other._thread)), _error(std::move(other._error)) {}

AsyncHttpServer& AsyncHttpServer::operator=(AsyncHttpServer&& other) noexcept {
  if (this != &other) {
    stop();
    _server = std::move(other._server);
    _thread = std::move(other._thread);
    _error = std::move(other._error);
  }
  return *this;
}

AsyncHttpServer::~AsyncHttpServer() { stop(); }

void AsyncHttpServer::setParserErrorCallback(HttpServer::ParserErrorCallback cb) {
  _server.setParserErrorCallback(std::move(cb));
}
void AsyncHttpServer::setMetricsCallback(HttpServer::MetricsCallback cb) { _server.setMetricsCallback(std::move(cb)); }

void AsyncHttpServer::setExpectationHandler(HttpServer::ExpectationHandler handler) {
  _server.setExpectationHandler(std::move(handler));
}

void AsyncHttpServer::setMiddlewareMetricsCallback(HttpServer::MiddlewareMetricsCallback cb) {
  _server.setMiddlewareMetricsCallback(std::move(cb));
}

void AsyncHttpServer::start() {
  ensureStartable();
  _thread = std::jthread([this](const std::stop_token& st) {
    try {
      _server.runUntil([&]() { return st.stop_requested(); });
    } catch (...) {
      _error = std::current_exception();
    };
  });
}

void AsyncHttpServer::startAndStopWhen(std::function<bool()> predicate) {
  ensureStartable();
  _thread = std::jthread([this, pred = std::move(predicate)](std::stop_token st) mutable {
    try {
      _server.runUntil([st = std::move(st), pred = std::move(pred)]() { return st.stop_requested() || pred(); });
    } catch (...) {
      _error = std::current_exception();
    }
  });
}

void AsyncHttpServer::startWithStopToken(std::stop_token token) {
  ensureStartable();
  // Launch a jthread that will stop when either the thread's own token is
  // requested or when the provided token is requested. We create a thread and
  // inside forward the provided token to the server runUntil predicate.
  _thread = std::jthread([this, token = std::move(token)](std::stop_token st) mutable {
    try {
      _server.runUntil(
          [st = std::move(st), token = std::move(token)]() { return st.stop_requested() || token.stop_requested(); });
    } catch (...) {
      _error = std::current_exception();
    }
  });
}

void AsyncHttpServer::stop() noexcept {
  if (_thread.joinable()) {
    _thread.request_stop();
    _thread.join();
  }
}

void AsyncHttpServer::beginDrain(std::chrono::milliseconds maxWait) noexcept { _server.beginDrain(maxWait); }

void AsyncHttpServer::rethrowIfError() {
  if (_error) {
    std::rethrow_exception(_error);
  }
}

void AsyncHttpServer::ensureStartable() {
  if (_thread.joinable()) {
    throw std::logic_error("AsyncHttpServer already started");
  }
}

}  // namespace aeronet
