#include "aeronet/async-http-server.hpp"

#include <exception>
#include <functional>
#include <stdexcept>
#include <stop_token>  // std::stop_token
#include <thread>
#include <utility>  // std::move

#include "aeronet/http-server.hpp"

namespace aeronet {

AsyncHttpServer::AsyncHttpServer(HttpServer server) noexcept : _server(std::move(server)) {}

AsyncHttpServer::AsyncHttpServer(AsyncHttpServer&& other) noexcept
    : _server(std::move(other._server)), _thread(std::move(other._thread)), _error(std::move(other._error)) {}

AsyncHttpServer& AsyncHttpServer::operator=(AsyncHttpServer&& other) noexcept {
  if (this != &other) {
    stopAndJoin();
    _server = std::move(other._server);
    _thread = std::move(other._thread);
    _error = std::move(other._error);
  }
  return *this;
}

AsyncHttpServer::~AsyncHttpServer() { stopAndJoin(); }

void AsyncHttpServer::start() {
  ensureStartable();
  _thread = std::jthread([this](const std::stop_token& st) { runLoopNoPredicate(st); });
}

void AsyncHttpServer::requestStop() noexcept {
  if (_thread.joinable()) {
    _thread.request_stop();
    _server.stop();
  }
}

void AsyncHttpServer::stopAndJoin() noexcept {
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

void AsyncHttpServer::runLoopNoPredicate(const std::stop_token& st) noexcept {
  try {
    _server.runUntil([&]() { return st.stop_requested(); });
  } catch (...) {
    _error = std::current_exception();
  }
}

}  // namespace aeronet
