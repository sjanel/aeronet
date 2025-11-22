#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "aeronet/socket.hpp"

namespace aeronet::test {

struct CapturedOtlpRequest {
  [[nodiscard]] std::string headerValue(std::string_view name) const;

  std::string method;
  std::string path;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
};

class OtlpTestCollector {
 public:
  OtlpTestCollector();

  OtlpTestCollector(const OtlpTestCollector&) = delete;
  OtlpTestCollector(OtlpTestCollector&&) = delete;
  OtlpTestCollector& operator=(const OtlpTestCollector&) = delete;
  OtlpTestCollector& operator=(OtlpTestCollector&&) = delete;

  ~OtlpTestCollector();

  [[nodiscard]] uint16_t port() const noexcept { return _port; }
  [[nodiscard]] std::string endpointForTraces() const;
  [[nodiscard]] std::string endpointForPath(std::string_view path) const;

  CapturedOtlpRequest waitForRequest(std::chrono::milliseconds timeout);
  std::vector<CapturedOtlpRequest> drain();

 private:
  void run();
  bool acceptOnce();
  void handleClient(int clientFd);
  void recordRequest(CapturedOtlpRequest req);

  Socket _listen;
  uint16_t _port{0};
  std::jthread _thread;
  std::atomic_bool _stop{false};
  std::mutex _mutex;
  std::condition_variable _cv;
  std::deque<CapturedOtlpRequest> _requests;
};

}  // namespace aeronet::test
