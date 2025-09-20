#include "aeronet/event-loop.hpp"

#include <unistd.h>

#include <cerrno>

#include "invalid_argument_exception.hpp"

namespace aeronet {

EventLoop::EventLoop(int epollFlags) {
  _epollFd = epoll_create1(epollFlags);
  if (_epollFd < 0) {
    throw invalid_argument("epoll_create1 failed");
  }
  _events.resize(64);
}

EventLoop::~EventLoop() {
  if (_epollFd != -1) {
    ::close(_epollFd);
  }
}

EventLoop::EventLoop(EventLoop&& other) noexcept : _epollFd(other._epollFd), _events(std::move(other._events)) {
  other._epollFd = -1;
}

EventLoop& EventLoop::operator=(EventLoop&& other) noexcept {
  if (this != &other) {
    if (_epollFd != -1) {
      ::close(_epollFd);
    }
    _epollFd = other._epollFd;
    _events = std::move(other._events);
    other._epollFd = -1;
  }
  return *this;
}

bool EventLoop::add(int fd, uint32_t events) const {
  epoll_event ev{events, epoll_data_t{.fd = fd}};

  return epoll_ctl(_epollFd, EPOLL_CTL_ADD, fd, &ev) == 0;
}

bool EventLoop::mod(int fd, uint32_t events) const {
  epoll_event ev{events, epoll_data_t{.fd = fd}};

  return epoll_ctl(_epollFd, EPOLL_CTL_MOD, fd, &ev) == 0;
}
void EventLoop::del(int fd) const { epoll_ctl(_epollFd, EPOLL_CTL_DEL, fd, nullptr); }

int EventLoop::poll(int timeoutMs, const std::function<void(int, uint32_t)>& cb) {
  int nbReadyFds = epoll_wait(_epollFd, _events.data(), static_cast<int>(_events.size()), timeoutMs);
  if (nbReadyFds < 0) {
    if (errno == EINTR) {
      return 0;
    }
    return -1;
  }
  for (int fdPos = 0; fdPos < nbReadyFds; ++fdPos) {
    const auto& event = _events[static_cast<uint32_t>(fdPos)];

    cb(event.data.fd, event.events);
  }
  if (nbReadyFds == static_cast<int>(_events.size())) {
    _events.resize(_events.size() * 2);
  }
  return nbReadyFds;
}
}  // namespace aeronet
