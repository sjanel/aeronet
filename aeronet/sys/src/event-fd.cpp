#include "aeronet/event-fd.hpp"

#include "aeronet/system-error-message.hpp"

#ifdef AERONET_LINUX
#include <sys/eventfd.h>
#elifdef AERONET_MACOS
#include <unistd.h>
#elifdef AERONET_WINDOWS
// Socket pair — included via socket-ops.hpp
#endif

#include <cerrno>

#include "aeronet/base-fd.hpp"
#include "aeronet/errno-throw.hpp"
#include "aeronet/log.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/system-error.hpp"

#if defined(AERONET_MACOS) || defined(AERONET_WINDOWS)
#include "aeronet/socket-ops.hpp"  // SetPipeNonBlockingCloExec / CreateLocalSocketPair
#endif

namespace aeronet {

// ---- Linux: eventfd ----
#ifdef AERONET_LINUX

EventFd::EventFd() : _baseFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
  if (fd() == kInvalidHandle) {
    ThrowSystemError("Unable to create a new EventFd");
  }
  log::debug("EventFd fd # {} opened", fd());
}

void EventFd::send() const noexcept {
  static constexpr eventfd_t one = 1;
  const auto ret = ::eventfd_write(fd(), one);
  if (ret == kInvalidHandle) {
    auto savedErr = LastSystemError();
    if (savedErr != error::kWouldBlock) {
      log::error("Event fd send failed err={}: {}", savedErr, SystemErrorMessage(savedErr));
    }
  } else {
    log::trace("Event fd send succeeded");
  }
}

void EventFd::read() const noexcept {
  eventfd_t counterValue;
  const auto ret = ::eventfd_read(fd(), &counterValue);
  if (ret == kInvalidHandle) {
    auto savedErr = LastSystemError();
    if (savedErr != error::kWouldBlock) {
      log::error("Event fd read failed err={}: {}", savedErr, SystemErrorMessage(savedErr));
    }
  } else {
    log::trace("Event fd drained (value={})", static_cast<unsigned long long>(counterValue));
  }
}

// ---- macOS: pipe ----
#elifdef AERONET_MACOS

EventFd::EventFd() {
  NativeHandle fds[2];
  if (::pipe(fds) != 0) {
    ThrowSystemError("Unable to create pipe for EventFd");
  }
  _baseFd = BaseFd(fds[0]);   // read end
  _writeFd = BaseFd(fds[1]);  // write end

  SetPipeNonBlockingCloExec(fds[0], fds[1]);
  log::debug("EventFd pipe read={} write={} opened", _baseFd.fd(), _writeFd.fd());
}

void EventFd::send() const noexcept {
  static constexpr char one = 1;
  const auto ret = ::write(_writeFd.fd(), &one, sizeof(one));
  if (ret == kInvalidHandle) {
    auto savedErr = LastSystemError();
    if (savedErr != error::kWouldBlock) {
      log::error("EventFd pipe send failed err={}: {}", savedErr, SystemErrorMessage(savedErr));
    }
  } else {
    log::trace("EventFd pipe send succeeded");
  }
}

void EventFd::read() const noexcept {
  char buf[64];
  // Drain all pending bytes from the pipe
  while (true) {
    const auto ret = ::read(_baseFd.fd(), buf, sizeof(buf));
    if (ret <= 0) {
      break;
    }
  }
  log::trace("EventFd pipe drained");
}

// ---- Windows: loopback socket pair ----
#elifdef AERONET_WINDOWS

EventFd::EventFd() {
  NativeHandle readEnd;
  NativeHandle writeEnd;
  CreateLocalSocketPair(readEnd, writeEnd);
  _baseFd = BaseFd(readEnd);    // read end — registered in event loop
  _writeFd = BaseFd(writeEnd);  // write end — send() writes here

  log::debug("EventFd socket pair read={} write={} opened", static_cast<uintptr_t>(readEnd),
             static_cast<uintptr_t>(writeEnd));
}

void EventFd::send() const noexcept {
  static constexpr char one = 1;
  const auto ret = ::send(_writeFd.fd(), &one, sizeof(one), 0);
  if (ret == SOCKET_ERROR) {
    auto savedErr = LastSystemError();
    if (savedErr != error::kWouldBlock) {
      log::error("EventFd socket send failed err={}: {}", savedErr, SystemErrorMessage(savedErr));
    }
  } else {
    log::trace("EventFd socket send succeeded");
  }
}

void EventFd::read() const noexcept {
  char buf[64];
  // Drain all pending bytes from the socket pair
  while (::recv(_baseFd.fd(), buf, sizeof(buf), 0) > 0) {
  }
  log::trace("EventFd socket drained");
}

#endif

}  // namespace aeronet
