#pragma once

#include <cstring>

#ifdef AERONET_WINDOWS
#include <winsock2.h>  // SOCKET, INVALID_SOCKET, closesocket, WSAE*
#endif

namespace aeronet {

// ---------------------------------------------------------------------------
// SystemErrorMessage – human-readable description for an error code.
//   POSIX  : wraps std::strerror (safe for errno values).
//   Windows: uses FormatMessageA for system/Winsock error codes (10 000+),
//            falls back to std::strerror for CRT errno values.
//   The returned pointer is valid at least until the next call from the same
//   thread.  Always log the numeric code alongside the message.
// ---------------------------------------------------------------------------
#ifdef AERONET_WINDOWS
inline const char* SystemErrorMessage(int err) noexcept {
  // Winsock / Win32 error codes are >= 10000; std::strerror only knows CRT errno.
  if (err >= 10000) {
    static thread_local char buf[256];
    DWORD len = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                 static_cast<DWORD>(err), 0, buf, sizeof(buf), nullptr);
    // Strip trailing \r\n added by FormatMessage.
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
      buf[--len] = '\0';
    }
    if (len > 0) {
      return buf;
    }
    // FormatMessage failed; fall back to a generic message.
    return "Unknown system error";
  }
  return std::strerror(err);
}
#else
inline const char* SystemErrorMessage(int err) noexcept { return std::strerror(err); }
#endif

}  // namespace aeronet