#pragma once

#ifdef AERONET_WINDOWS
#include <winsock2.h>  // SOCKET, INVALID_SOCKET, closesocket, WSAE*
#else
#include <unistd.h>  // close
#endif

#include "aeronet/native-handle.hpp"

namespace aeronet {

// ---------------------------------------------------------------------------
// CloseNativeHandle – close a socket / file descriptor portably.
//   POSIX : ::close(fd)
//   Windows: ::closesocket(fd)
// Returns 0 on success, -1 / SOCKET_ERROR on failure.
// ---------------------------------------------------------------------------
#ifdef AERONET_WINDOWS
inline int CloseNativeHandle(NativeHandle fd) noexcept { return ::closesocket(fd); }
#else
inline int CloseNativeHandle(NativeHandle fd) noexcept { return ::close(fd); }
#endif

}  // namespace aeronet