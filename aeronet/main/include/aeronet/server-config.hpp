#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace aeronet {

struct ServerConfig {
  // Core socket / listener
  uint16_t port{8080};
  bool reusePort{false};

  // Limits & lifecycle
  std::size_t maxHeaderBytes{8192};
  std::size_t maxBodyBytes{1 << 20};  // 1MB
  uint32_t maxRequestsPerConnection{100};
  std::chrono::milliseconds keepAliveTimeout{std::chrono::milliseconds{5000}};
  bool enableKeepAlive{true};

  // Fluent builder style setters
  ServerConfig& withPort(uint16_t port) {
    this->port = port;
    return *this;
  }

  ServerConfig& withReusePort(bool on = true) {
    this->reusePort = on;
    return *this;
  }

  ServerConfig& withMaxHeaderBytes(std::size_t maxHeaderBytes) {
    this->maxHeaderBytes = maxHeaderBytes;
    return *this;
  }

  ServerConfig& withMaxBodyBytes(std::size_t maxBodyBytes) {
    this->maxBodyBytes = maxBodyBytes;
    return *this;
  }

  ServerConfig& withMaxRequestsPerConnection(uint32_t maxRequests) {
    this->maxRequestsPerConnection = maxRequests;
    return *this;
  }

  ServerConfig& withKeepAliveTimeout(std::chrono::milliseconds timeout) {
    this->keepAliveTimeout = timeout;
    return *this;
  }

  ServerConfig& withKeepAliveMode(bool on = true) {
    this->enableKeepAlive = on;
    return *this;
  }
};

}  // namespace aeronet
