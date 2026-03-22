#include "aeronet/accept-callouts.hpp"

#include "aeronet/native-handle.hpp"

#if defined(__GNUC__) || defined(__clang__)
#define AERONET_WEAK __attribute__((weak))
#else
#define AERONET_WEAK
#endif

// Default no-op: production code needs no post-accept hook.
extern "C" AERONET_WEAK void AeronetOnConnectionAccepted([[maybe_unused]] aeronet::NativeHandle fd) {}
