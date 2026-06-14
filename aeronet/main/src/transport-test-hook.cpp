
#include "aeronet/transport-test-hook.hpp"

#include <atomic>
#include <memory>
#include <utility>

#include "aeronet/transport.hpp"

namespace aeronet::test {
namespace {
std::atomic<TransportDecoratorFn> g_transportDecorator{nullptr};
}

std::unique_ptr<ITransport> ApplyTransportDecorator(std::unique_ptr<ITransport> transport) {
  if (auto decorator = g_transportDecorator.load(std::memory_order_acquire)) {
    return decorator(std::move(transport));
  }
  return transport;
}

ScopedTransportDecorator::ScopedTransportDecorator(TransportDecoratorFn fn) {
  g_transportDecorator.store(fn, std::memory_order_release);
}

ScopedTransportDecorator::~ScopedTransportDecorator() {
  g_transportDecorator.store(nullptr, std::memory_order_release);
}

}  // namespace aeronet::test
