#include "aeronet/io-callouts.hpp"

#include "aeronet/native-handle.hpp"

#if defined(__GNUC__) || defined(__clang__)
#define AERONET_WEAK __attribute__((weak))
#else
#define AERONET_WEAK
#endif

// Default: always use io_uring data I/O when available.
extern "C" AERONET_WEAK bool AeronetUseIoRingForFd([[maybe_unused]] aeronet::NativeHandle) { return true; }
