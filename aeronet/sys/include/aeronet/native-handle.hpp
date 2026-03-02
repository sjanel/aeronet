#pragma once

#ifdef AERONET_WINDOWS
#include <winsock2.h>  // SOCKET
#endif

namespace aeronet {

// ---------------------------------------------------------------------------
// NativeHandle – the OS-level socket / file descriptor type.
//   POSIX : int            (file descriptors)
//   Windows : SOCKET       (unsigned pointer-sized integer)
// ---------------------------------------------------------------------------
#ifdef AERONET_WINDOWS
using NativeHandle = SOCKET;
inline constexpr NativeHandle kInvalidHandle = INVALID_SOCKET;
#else
using NativeHandle = int;
inline constexpr NativeHandle kInvalidHandle = -1;
#endif

}  // namespace aeronet