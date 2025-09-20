#pragma once

#include <sys/epoll.h>

#include <cstdint>
#include <functional>

#include "vector.hpp"

namespace aeronet {

class EventLoop {
 public:
  explicit EventLoop(int epollFlags = 0);

  ~EventLoop();

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;
  EventLoop(EventLoop&& other) noexcept;
  EventLoop& operator=(EventLoop&& other) noexcept;

  [[nodiscard]] bool add(int fd, uint32_t events) const;

  [[nodiscard]] bool mod(int fd, uint32_t events) const;

  void del(int fd) const;

  int poll(int timeoutMs, const std::function<void(int fd, uint32_t ev)>& cb);

 private:
  int _epollFd{-1};
  vector<epoll_event> _events;
};

}  // namespace aeronet
