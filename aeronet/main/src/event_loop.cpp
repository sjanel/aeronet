#include "aeronet/event_loop.hpp"

#include <unistd.h>

#include <cerrno>
#include <stdexcept>

namespace aeronet {

EventLoop::EventLoop() {
  epollFd_ = epoll_create1(0);
  if (epollFd_ < 0) {
    throw std::runtime_error("epoll_create1 failed");
  }
  events_.resize(64);
}

EventLoop::~EventLoop() {
  if (epollFd_ != -1) {
    ::close(epollFd_);
  }
}

bool EventLoop::add(int fd, uint32_t events) {
  epoll_event ev{events, epoll_data_t{.fd = fd}};

  return epoll_ctl(epollFd_, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool EventLoop::mod(int fd, uint32_t events) {
  epoll_event ev{events, epoll_data_t{.fd = fd}};
  return epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &ev) == 0;
}
void EventLoop::del(int fd) { epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr); }

int EventLoop::poll(int timeoutMs, const std::function<void(int, uint32_t)>& cb) {
  int n = epoll_wait(epollFd_, events_.data(), (int)events_.size(), timeoutMs);
  if (n < 0) {
    if (errno == EINTR) return 0;
    return -1;
  }
  for (int i = 0; i < n; ++i) {
    cb(events_[(unsigned)i].data.fd, events_[(unsigned)i].events);
  }
  if (n == (int)events_.size()) events_.resize(events_.size() * 2);
  return n;
}
}  // namespace aeronet
