#include <benchmark/benchmark.h>

#include <cstddef>
#include <memory>
#include <string_view>
#include <utility>

#include "aeronet/compiler-config.hpp"
#include "aeronet/transport.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/ssl.h>

#include "aeronet/tls-transport.hpp"
#endif

namespace aeronet {
namespace {

class VirtualCapabilities {
 public:
  virtual ~VirtualCapabilities() = default;

  [[nodiscard]] virtual bool supportsSendfile() const noexcept = 0;
  [[nodiscard]] virtual bool handshakeDone() const noexcept = 0;
  [[nodiscard]] virtual bool hasPendingReadData() const noexcept = 0;
  virtual TransportResult write(std::string_view data) = 0;
};

class VirtualPlainCapabilities final : public VirtualCapabilities {
 public:
  [[nodiscard]] bool supportsSendfile() const noexcept override { return true; }
  [[nodiscard]] bool handshakeDone() const noexcept override { return true; }
  [[nodiscard]] bool hasPendingReadData() const noexcept override { return false; }
  TransportResult write(std::string_view data) override { return _backend.write(data); }

 private:
  PlainTransport _backend{kInvalidHandle, ZerocopyMode::Disabled, 0};
};

#ifdef AERONET_ENABLE_OPENSSL
class VirtualTlsCapabilities final : public VirtualCapabilities {
 public:
  [[nodiscard]] bool supportsSendfile() const noexcept override { return false; }
  [[nodiscard]] bool handshakeDone() const noexcept override { return false; }
  [[nodiscard]] bool hasPendingReadData() const noexcept override { return false; }
  TransportResult write(std::string_view data) override { return {data.size(), TransportHint::None}; }
};

class ErasedTlsWriteBackend final : public TransportBackend<ErasedTlsWriteBackend, TransportKind::Tls> {
 public:
  TransportResult read([[maybe_unused]] char* buf, [[maybe_unused]] std::size_t len) {
    return {0, TransportHint::ReadReady};
  }

  TransportResult write(std::string_view data) { return {data.size(), TransportHint::None}; }
};
#endif

AERONET_NOINLINE void ReadVirtualCapabilities(const VirtualCapabilities& capabilities) {
  benchmark::DoNotOptimize(capabilities.supportsSendfile());
  benchmark::DoNotOptimize(capabilities.handshakeDone());
  benchmark::DoNotOptimize(capabilities.hasPendingReadData());
}

AERONET_NOINLINE void ReadTransportCapabilities(const Transport& transport) {
  benchmark::DoNotOptimize(transport.supportsSendfile());
  benchmark::DoNotOptimize(transport.handshakeDone());
  benchmark::DoNotOptimize(transport.hasPendingReadData());
}

AERONET_NOINLINE TransportResult WriteVirtual(VirtualCapabilities& transport) {
  return transport.write(std::string_view{});
}

AERONET_NOINLINE TransportResult WriteTransport(Transport& transport) { return transport.write(std::string_view{}); }

void BM_VirtualPlainCapabilities(benchmark::State& state) {
  const VirtualPlainCapabilities capabilities;
  const VirtualCapabilities* dispatch = &capabilities;
  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(dispatch);
    ReadVirtualCapabilities(*dispatch);
  }
  state.SetItemsProcessed(state.iterations() * 3);
}

BENCHMARK(BM_VirtualPlainCapabilities);

void BM_InlinePlainCapabilities(benchmark::State& state) {
  const Transport transport(kInvalidHandle, ZerocopyMode::Disabled, 0);
  const Transport* dispatch = &transport;
  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(dispatch);
    ReadTransportCapabilities(*dispatch);
  }
  state.SetItemsProcessed(state.iterations() * 3);
}

BENCHMARK(BM_InlinePlainCapabilities);

void BM_VirtualPlainWrite(benchmark::State& state) {
  VirtualPlainCapabilities backend;
  VirtualCapabilities* dispatch = &backend;
  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(dispatch);
    benchmark::DoNotOptimize(WriteVirtual(*dispatch));
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_VirtualPlainWrite);

void BM_InlinePlainWrite(benchmark::State& state) {
  Transport transport(kInvalidHandle, ZerocopyMode::Disabled, 0);
  Transport* dispatch = &transport;
  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(dispatch);
    benchmark::DoNotOptimize(WriteTransport(*dispatch));
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_InlinePlainWrite);

void BM_VirtualPlainLifetime(benchmark::State& state) {
  for ([[maybe_unused]] auto iteration : state) {
    std::unique_ptr<VirtualCapabilities> transport = std::make_unique<VirtualPlainCapabilities>();
    benchmark::DoNotOptimize(transport);
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_VirtualPlainLifetime);

void BM_InlinePlainLifetime(benchmark::State& state) {
  for ([[maybe_unused]] auto iteration : state) {
    Transport transport(kInvalidHandle, ZerocopyMode::Disabled, 0);
    benchmark::DoNotOptimize(transport);
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_InlinePlainLifetime);

#ifdef AERONET_ENABLE_OPENSSL
void BM_VirtualTlsCapabilities(benchmark::State& state) {
  const VirtualTlsCapabilities capabilities;
  const VirtualCapabilities* dispatch = &capabilities;
  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(dispatch);
    ReadVirtualCapabilities(*dispatch);
  }
  state.SetItemsProcessed(state.iterations() * 3);
}

BENCHMARK(BM_VirtualTlsCapabilities);

void BM_ErasedTlsCapabilities(benchmark::State& state) {
  TlsTransport::SslPtr ssl(nullptr, &::SSL_free);
  const Transport transport(std::make_unique<TlsTransport>(std::move(ssl), 0));
  const Transport* dispatch = &transport;
  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(dispatch);
    ReadTransportCapabilities(*dispatch);
  }
  state.SetItemsProcessed(state.iterations() * 3);
}

BENCHMARK(BM_ErasedTlsCapabilities);

void BM_VirtualTlsWrite(benchmark::State& state) {
  VirtualTlsCapabilities backend;
  VirtualCapabilities* dispatch = &backend;
  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(dispatch);
    benchmark::DoNotOptimize(WriteVirtual(*dispatch));
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_VirtualTlsWrite);

void BM_ErasedTlsWrite(benchmark::State& state) {
  Transport transport(std::make_unique<ErasedTlsWriteBackend>());
  Transport* dispatch = &transport;
  for ([[maybe_unused]] auto iteration : state) {
    benchmark::DoNotOptimize(dispatch);
    benchmark::DoNotOptimize(WriteTransport(*dispatch));
  }
  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_ErasedTlsWrite);
#endif

}  // namespace
}  // namespace aeronet

BENCHMARK_MAIN();
