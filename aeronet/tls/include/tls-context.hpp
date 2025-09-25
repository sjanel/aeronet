#pragma once

#include <memory>

#include "aeronet/tls-config.hpp"
#include "raw-bytes.hpp"

// Forward declare OpenSSL context struct (avoid pulling heavy headers into public interface).
struct ssl_ctx_st;  // SSL_CTX

namespace aeronet {

// Forward-declared metrics container (owned by HttpServer) used for ALPN mismatch counting during selection.
struct TlsMetricsExternal {
  uint64_t alpnStrictMismatches{0};
};

// Forward declare the OpenSSL free function (signature matches OpenSSL); avoids including heavy headers here.

// RAII wrapper around SSL_CTX with minimal configuration derived from HttpServerConfig::TLSConfig.
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

  [[nodiscard]] void* raw() const noexcept { return reinterpret_cast<void*>(_ctx.get()); }

 private:
  struct AlpnData {  // private implementation detail (binary length-prefixed ALPN protocol list per RFC 7301)
    RawBytes wire;   // [len][bytes]...[len][bytes]
    bool mustMatch{false};
    TlsMetricsExternal* metrics{nullptr};
  };
  struct CtxDel {
    void operator()(ssl_ctx_st* ctxPtr) const noexcept;
  };
  using CtxPtr = std::unique_ptr<ssl_ctx_st, CtxDel>;

  CtxPtr _ctx;
  // alpnData is a unique_ptr because the pointer value should stay valid as passed to SSL_CTX_set_alpn_select_cb
  // callback. If TlsContext is moved around, not having a unique_ptr would invalid the pointer passed to the callback
  std::unique_ptr<AlpnData> _alpnData;
};

}  // namespace aeronet