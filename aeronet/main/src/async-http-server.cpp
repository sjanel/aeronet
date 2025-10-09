#include "aeronet/async-http-server.hpp"

#include <exception>
#include <functional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

#include "aeronet/http-server.hpp"

namespace aeronet {

AsyncHttpServer::AsyncHttpServer(HttpServerConfig httpServerConfig) : _server(std::move(httpServerConfig)) {}

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

void AsyncHttpServer::requestStop() noexcept {
  if (_thread.joinable()) {
    _thread.request_stop();
  }
}

void AsyncHttpServer::stop() noexcept {
  if (_thread.joinable()) {
    requestStop();
    _thread.join();
  }
}

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
