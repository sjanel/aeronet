#include "aeronet/io-callouts.hpp"

#include "aeronet/native-handle.hpp"

#if defined(__GNUC__) || defined(__clang__)
#define AERONET_WEAK __attribute__((weak))
#else
#define AERONET_WEAK
#endif

// Default: do NOT use io_uring for synchronous data I/O.
// The synchronous submit-and-wait pattern (one io_uring_enter per recv/send)
// is strictly slower than direct ::recv/::send. The real win for io_uring on
// the data path requires a fully asynchronous proactor design (separate code path).
extern "C" AERONET_WEAK bool AeronetUseIoRingForFd([[maybe_unused]] aeronet::NativeHandle) { return false; }
