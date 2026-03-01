#include "aeronet/base-fd.hpp"

#include <cstring>
#include <utility>

#include "aeronet/log.hpp"
#include "aeronet/platform.hpp"

#ifdef AERONET_POSIX
#include <unistd.h>
#endif

namespace aeronet {

BaseFd& BaseFd::operator=(BaseFd&& other) noexcept {
  if (this != &other) {
    close();
    _fd = other.release();
#ifdef AERONET_WINDOWS
    _kind = other._kind;
#endif
  }
  return *this;
}

void BaseFd::close() noexcept {
  if (_fd != kClosedFd) {
    while (true) {
#ifdef AERONET_WINDOWS
      if (_kind == HandleKind::Win32Handle) {
        // Win32 kernel objects (Event, WaitableTimer) must use CloseHandle.
        if (::CloseHandle(reinterpret_cast<HANDLE>(_fd))) {
          break;
        }
        log::error("CloseHandle {} failed: error {}", static_cast<uintptr_t>(_fd), ::GetLastError());
      } else {
        if (::closesocket(_fd) == 0) {
          break;
        }
        log::error("closesocket handle {} failed: error {}", static_cast<uintptr_t>(_fd), LastSystemError());
      }
      break;
#else
      if (::close(_fd) == 0) {
        // success
        break;
      }
      const int err = LastSystemError();
      if (err == error::kInterrupted) {
        // Retry close if interrupted; POSIX allows either retry or treat as closed.
        continue;
      }
      // Other errors: EBADF (benign if race closed elsewhere), ENOSPC (should not happen here), etc.
      log::error("close fd # {} failed: {}", _fd, SystemErrorMessage(err));
      break;
#endif
    }
    log::debug("fd # {} closed", static_cast<intptr_t>(_fd));
    _fd = kClosedFd;
  }
}

NativeHandle BaseFd::release() noexcept { return std::exchange(_fd, kClosedFd); }

}  // namespace aeronet