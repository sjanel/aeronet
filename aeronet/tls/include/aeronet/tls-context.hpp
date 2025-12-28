#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/tls-config.hpp"

// Forward declare OpenSSL context structs (avoid pulling heavy headers into public interface).
struct ssl_ctx_st;  // SSL_CTX
struct ssl_st;      // SSL

namespace aeronet {

class TlsTicketKeyStore;

// Forward declare the OpenSSL free function (signature matches OpenSSL); avoids including heavy headers here.

// RAII wrapper around SSL_CTX with minimal configuration derived from HttpServerConfig::TLSConfig.
class TlsContext {
 public:
  TlsContext() = default;

  // Creates a new TLSContext
  TlsContext(const TLSConfig& cfg, std::shared_ptr<TlsTicketKeyStore> ticketKeyStore = {});

  TlsContext(const TlsContext&) = delete;
  TlsContext& operator=(const TlsContext&) = delete;
  // TLSContext is not movable to keep the internal pointer stable for OpenSSL callbacks.
  // Make all fields passed to callbacks stable if you need to add move support.
  TlsContext(TlsContext&&) noexcept = delete;
  TlsContext& operator=(TlsContext&&) noexcept = delete;

  ~TlsContext();

  [[nodiscard]] void* raw() const noexcept { return static_cast<void*>(_ctx.get()); }

  [[nodiscard]] uint64_t alpnStrictMismatches() const noexcept { return _alpnData.nbStrictMismatches; }

 private:
  struct AlpnData {
    // private implementation detail (binary length-prefixed ALPN protocol list per RFC 7301)
    RawBytes32 wire;  // [len][bytes]...[len][bytes]
    uint64_t nbStrictMismatches{0};
    bool mustMatch{false};
  };
  struct CtxDel {
    void operator()(ssl_ctx_st* ctxPtr) const noexcept;
  };
  using CtxPtr = std::unique_ptr<ssl_ctx_st, CtxDel>;

  struct SniRoute {
    RawChars32 pattern;
    bool wildcard{false};
    CtxPtr ctx;
  };

  struct SniRoutes {
    std::unique_ptr<SniRoute[]> routes;
    std::size_t nbRoutes;
  };

  static int SelectSniRoute(ssl_st* ssl, int* alert, void* arg);
  static int SelectAlpn(ssl_st* ssl, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                        unsigned int inlen, void* arg);

  CtxPtr _ctx;
  // alpnData is a unique_ptr because the pointer value should stay valid as passed to SSL_CTX_set_alpn_select_cb
  // callback. If TlsContext is moved around, not having a unique_ptr would invalid the pointer passed to the callback
  AlpnData _alpnData;
  SniRoutes _sniRoutes;
  std::shared_ptr<TlsTicketKeyStore> _ticketKeyStore;
};

}  // namespace aeronet