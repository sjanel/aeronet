#include "aeronet/io-callouts.hpp"

#include "aeronet/native-handle.hpp"

#if defined(__GNUC__) || defined(__clang__)
#define AERONET_WEAK __attribute__((weak))
#else
#define AERONET_WEAK
#endif

// Default: allow ring-based data I/O (async recv/send proactor) for every fd.
// Test binaries link their own definition (see sys-test-support.hpp) returning false for
// fds that have pending error-injection actions, so the server falls back to the
// synchronous transport path where the userspace write()/writev()/sendfile() overrides
// can intercept the calls.
extern "C" AERONET_WEAK bool AeronetUseIoRingForFd([[maybe_unused]] aeronet::NativeHandle) { return true; }
