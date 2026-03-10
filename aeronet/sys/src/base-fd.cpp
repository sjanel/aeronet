#include "aeronet/base-fd.hpp"

#include <cstdint>
#include <cstring>
#include <utility>

#include "aeronet/log.hpp"
#include "aeronet/system-error-message.hpp"
#include "aeronet/system-error.hpp"

#ifdef AERONET_POSIX
#include <unistd.h>
#endif
#ifdef AERONET_WINDOWS
#include <io.h>
#endif

#include "aeronet/native-handle.hpp"

namespace aeronet {

BaseFd& BaseFd::operator=(BaseFd&& other) noexcept {
  if (this != &other) {
    close();
    _fd = other.release();
#ifdef AERONET_MACOS
    _owns = other._owns;
#endif
#ifdef AERONET_WINDOWS
    _kind = other._kind;
#endif
  }
  return *this;
}

void BaseFd::close() noexcept {
  if (_fd != kClosedFd) {
#ifdef AERONET_MACOS
    if (!_owns) {
      // Non-owning (borrowed) fd — do not close, just forget.
      log::debug("fd # {} released (borrowed, not closed)", static_cast<intptr_t>(_fd));
      _fd = kClosedFd;
      return;
    }
#endif
    while (true) {
#ifdef AERONET_WINDOWS
      if (_kind == HandleKind::CrtFd) {
        if (::_close(static_cast<int>(_fd)) == 0) {
          break;
        }
        log::error("_close crt fd {} failed: error {}: {}", static_cast<int>(_fd), errno, SystemErrorMessage(errno));
        break;
      }
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