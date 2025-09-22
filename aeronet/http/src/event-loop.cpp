#include "event-loop.hpp"

#include <sys/epoll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <utility>

#include "invalid_argument_exception.hpp"
#include "log.hpp"
#include "timedef.hpp"

namespace aeronet {

EventLoop::EventLoop(int epollFlags) {
  _epollFd = epoll_create1(epollFlags);
  if (_epollFd < 0) {
    throw invalid_argument("epoll_create1 failed");
  }
  _events.resize(64);
}

EventLoop::~EventLoop() { close(); }

EventLoop::EventLoop(EventLoop&& other) noexcept
    : _epollFd(std::exchange(other._epollFd, -1)), _events(std::move(other._events)) {}

EventLoop& EventLoop::operator=(EventLoop&& other) noexcept {
  if (this != &other) {
    close();
    _epollFd = std::exchange(other._epollFd, -1);
    _events = std::move(other._events);
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

int EventLoop::poll(Duration timeout, const std::function<void(int, uint32_t)>& cb) {
  const auto timeoutMs = std::chrono::duration_cast<std::chrono::milliseconds>(timeout).count();
  int timeoutEpollWait;
  if (timeoutMs > static_cast<std::remove_const_t<decltype(timeoutMs)>>(std::numeric_limits<int>::max())) {
    log::warn("Timeout value is too large, clamping to max int");
    timeoutEpollWait = std::numeric_limits<int>::max();
  } else {
    timeoutEpollWait = static_cast<int>(timeoutMs);
  }
  int nbReadyFds = epoll_wait(_epollFd, _events.data(), static_cast<int>(_events.size()), timeoutEpollWait);
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

void EventLoop::close() {
  if (_epollFd != -1) {
    ::close(_epollFd);
    _epollFd = -1;
  }
}

}  // namespace aeronet
