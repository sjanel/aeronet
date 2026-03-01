#include "aeronet/event-fd.hpp"

#include "aeronet/platform.hpp"

#ifdef AERONET_LINUX
#include <sys/eventfd.h>
#elifdef AERONET_MACOS
#include <unistd.h>
#elifdef AERONET_WINDOWS
// Windows event via CreateEventW — included via platform.hpp
#endif

#include <cerrno>

#include "aeronet/base-fd.hpp"
#include "aeronet/errno-throw.hpp"
#include "aeronet/log.hpp"

#ifdef AERONET_MACOS
#include "aeronet/socket-ops.hpp"  // SetPipeNonBlockingCloExec
#endif

namespace aeronet {

// ---- Linux: eventfd ----
#ifdef AERONET_LINUX

EventFd::EventFd() : _baseFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
  if (fd() == -1) {
    throw_errno("Unable to create a new EventFd");
  }
  log::debug("EventFd fd # {} opened", fd());
}

void EventFd::send() const noexcept {
  static constexpr eventfd_t one = 1;
  const auto ret = ::eventfd_write(fd(), one);
  if (ret == -1) {
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
  if (ret == -1) {
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
  int fds[2];
  if (::pipe(fds) != 0) {
    throw_errno("Unable to create pipe for EventFd");
  }
  _baseFd = BaseFd(fds[0]);   // read end
  _writeFd = BaseFd(fds[1]);  // write end

  SetPipeNonBlockingCloExec(fds[0], fds[1]);
  log::debug("EventFd pipe read={} write={} opened", _baseFd.fd(), _writeFd.fd());
}

void EventFd::send() const noexcept {
  static constexpr char one = 1;
  const auto ret = ::write(_writeFd.fd(), &one, sizeof(one));
  if (ret == -1) {
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

// ---- Windows: Event object stub ----
#elifdef AERONET_WINDOWS

EventFd::EventFd() {
  // Use a manual-reset event. The HANDLE is stored as NativeHandle in BaseFd.
  HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (ev == nullptr) {
    auto err = ::GetLastError();
    log::error("CreateEventW failed (error={})", err);
    throw std::runtime_error("Unable to create EventFd on Windows");
  }
  _baseFd = BaseFd(reinterpret_cast<NativeHandle>(ev), BaseFd::HandleKind::Win32Handle);
  log::debug("EventFd Windows event handle created");
}

void EventFd::send() const noexcept {
  ::SetEvent(reinterpret_cast<HANDLE>(_baseFd.fd()));
  log::trace("EventFd Windows event signaled");
}

void EventFd::read() const noexcept {
  ::ResetEvent(reinterpret_cast<HANDLE>(_baseFd.fd()));
  log::trace("EventFd Windows event reset");
}

#endif

}  // namespace aeronet