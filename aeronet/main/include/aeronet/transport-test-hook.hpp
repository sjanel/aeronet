#pragma once

#include <memory>

#include "aeronet/transport.hpp"

namespace aeronet::test {

/// Function pointer type for decorating/wrapping transports.
/// Takes ownership of the input transport, returns the transport to use.
using TransportDecoratorFn = std::unique_ptr<ITransport> (*)(std::unique_ptr<ITransport>);

/// Apply the currently registered decorator (if any) to a transport.
/// Called by connection-manager at accept time.
std::unique_ptr<ITransport> ApplyTransportDecorator(std::unique_ptr<ITransport> transport);

/// RAII guard that sets/clears the global transport decorator.
class ScopedTransportDecorator {
 public:
  explicit ScopedTransportDecorator(TransportDecoratorFn fn);

  ScopedTransportDecorator(const ScopedTransportDecorator&) = delete;
  ScopedTransportDecorator(ScopedTransportDecorator&&) noexcept = delete;

  ScopedTransportDecorator& operator=(const ScopedTransportDecorator&) = delete;
  ScopedTransportDecorator& operator=(ScopedTransportDecorator&&) noexcept = delete;

  ~ScopedTransportDecorator();
};

}  // namespace aeronet::test
