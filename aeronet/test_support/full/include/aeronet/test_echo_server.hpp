#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "aeronet/socket.hpp"

namespace aeronet::test {

// RAII wrapper for a simple echo server. Joins the echo thread on destruction so that
// the thread never outlives the caller's scope (prevents use-after-free of global loggers
// during process exit).
struct EchoServer {
  EchoServer() = default;

  EchoServer(Socket sock, uint16_t pt, std::shared_ptr<std::atomic<bool>> sf, std::thread thr);

  EchoServer(EchoServer&&) noexcept = default;
  EchoServer(const EchoServer&) = delete;
  EchoServer& operator=(EchoServer&&) noexcept = default;
  EchoServer& operator=(const EchoServer&) = delete;

  ~EchoServer();

  Socket listenSocket;
  uint16_t port{};
  std::shared_ptr<std::atomic<bool>> stopFlag;
  std::thread echoThread;
};

// Start a simple echo server bound to loopback on an ephemeral port. Throws std::system_error on error.
EchoServer startEchoServer();

}  // namespace aeronet::test
