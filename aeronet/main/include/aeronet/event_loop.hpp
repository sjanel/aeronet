#pragma once
#include <sys/epoll.h>

#include <cstdint>
#include <functional>

#include "vector.hpp"

namespace aeronet {

class EventLoop {
 public:
  EventLoop();
  ~EventLoop();
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;
  EventLoop(EventLoop&&) = delete;
  EventLoop& operator=(EventLoop&&) = delete;
  bool add(int fd, uint32_t events);
  bool mod(int fd, uint32_t events);
  void del(int fd);
  int poll(int timeoutMs, const std::function<void(int fd, uint32_t ev)>& cb);

 private:
  int epollFd_{-1};
  vector<epoll_event> events_;
};

}  // namespace aeronet
