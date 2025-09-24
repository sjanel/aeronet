#pragma once

#include <memory>
#include <string>

#include "aeronet/tls-config.hpp"

// Forward declare OpenSSL context struct (avoid pulling heavy headers into public interface).
struct ssl_ctx_st;  // SSL_CTX

namespace aeronet {

// Forward-declared metrics container (owned by HttpServer) used for ALPN mismatch counting during selection.
struct TlsMetricsExternal {
  uint64_t alpnStrictMismatches{0};
};

// Forward declare the OpenSSL free function (signature matches OpenSSL); avoids including heavy headers here.

// RAII wrapper around SSL_CTX with minimal configuration derived from ServerConfig::TLSConfig.
class TlsContext {
 public:
  // Creates a new TLSContext
  TlsContext(const TLSConfig& cfg, TlsMetricsExternal* metrics);

  TlsContext() = default;

  TlsContext(const TlsContext&) = delete;
  TlsContext& operator=(const TlsContext&) = delete;
  TlsContext(TlsContext&&) noexcept = default;
  TlsContext& operator=(TlsContext&&) noexcept = default;

  ~TlsContext();

  [[nodiscard]] bool valid() const noexcept { return _ctx != nullptr; }
  [[nodiscard]] void* raw() const noexcept { return reinterpret_cast<void*>(_ctx.get()); }

 private:
  struct AlpnData {  // private implementation detail
    std::string wire;
    bool mustMatch{false};
    TlsMetricsExternal* metrics{nullptr};
  };
  struct CtxDel {
    void operator()(ssl_ctx_st* ctxPtr) const noexcept;
  };
  using CtxPtr = std::unique_ptr<ssl_ctx_st, CtxDel>;
  explicit TlsContext(CtxPtr ctx, std::unique_ptr<AlpnData> alpn);
  CtxPtr _ctx;                          // empty by default
  std::unique_ptr<AlpnData> _alpnData;  // nullptr if ALPN disabled
};

}  // namespace aeronet