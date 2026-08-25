#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "aeronet/object-array-pool.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/tls-config.hpp"

// Forward declare OpenSSL context structs (avoid pulling heavy headers into public interface).
struct ssl_ctx_st;  // SSL_CTX
struct ssl_st;      // SSL
struct x509_store_ctx_st;

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
    std::string_view pattern;
    bool wildcard{false};
    CtxPtr ctx;
    std::span<const std::byte> ocspResponse;
  };

  struct SniRoutes {
    std::unique_ptr<SniRoute[]> routes;
    std::size_t nbRoutes{0};
    ObjectArrayPool<char> charStorage;
  };

  static int SelectSniRoute(ssl_st* ssl, int* alert, void* arg);
  static int SelectAlpn(ssl_st* ssl, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                        unsigned int inlen, void* arg);
  static int StapleOcspResponse(ssl_st* ssl, void* arg);
  static int VerifyPeerCertificate(int preverifyOk, x509_store_ctx_st* storeCtx);
  static void LogSessionKeys(const ssl_st* ssl, const char* line);

  struct RevocationData {
    TlsRevocationCallback callback{nullptr};
    void* userContext{nullptr};
  };

  class KeyLogWriter;

  // Callback arguments are declared before the contexts so they outlive every SSL_CTX during normal destruction and
  // constructor unwinding. TlsContext is non-movable, which keeps their addresses stable.
  AlpnData _alpnData;
  RawBytes32 _ocspResponse;
  RevocationData _revocationData;
  std::unique_ptr<KeyLogWriter> _keyLogWriter;
  std::shared_ptr<TlsTicketKeyStore> _ticketKeyStore;
  SniRoutes _sniRoutes;
  CtxPtr _ctx;
};

}  // namespace aeronet
