#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "aeronet/object-array-pool.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/single-writer-counter.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/vector.hpp"

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

  // Creates a new TLSContext. Throws if the configuration or one of its files (certificate, private key, OCSP response,
  // CRL) is invalid or cannot be read, leaving the OpenSSL error queue of the calling thread empty.
  TlsContext(const TLSConfig& cfg, std::shared_ptr<TlsTicketKeyStore> ticketKeyStore = {});

  TlsContext(const TlsContext&) = delete;
  TlsContext& operator=(const TlsContext&) = delete;
  // TLSContext is not movable to keep the internal pointer stable for OpenSSL callbacks.
  // Make all fields passed to callbacks stable if you need to add move support.
  TlsContext(TlsContext&&) noexcept = delete;
  TlsContext& operator=(TlsContext&&) noexcept = delete;

  ~TlsContext();

  [[nodiscard]] void* raw() const noexcept { return static_cast<void*>(_ctx.get()); }

  [[nodiscard]] uint64_t alpnStrictMismatches() const noexcept { return _alpnData.nbStrictMismatches.load(); }

  // The session ticket key store of this context, null when session tickets are disabled.
  [[nodiscard]] const std::shared_ptr<TlsTicketKeyStore>& ticketKeyStore() const noexcept { return _ticketKeyStore; }

  // Returns the path of a file read to build this context (certificate, private key, OCSP response or CRL file, SNI
  // ones included) that was rewritten, replaced (renamed over, symbolic link swapped) or removed since, or an empty
  // string if none was. Costs one stat() per file: meant for configuration reloads, never for the hot path.
  [[nodiscard]] std::string_view changedInputFile() const;

 private:
  // Identity and version of a file, from a stat() following symbolic links. All zero when the file cannot be stat'ed.
  struct FileStamp {
    bool operator==(const FileStamp&) const noexcept = default;

    uint64_t device{};
    uint64_t inode{};
    uint64_t size{};
    int64_t mtimeNs{};
    int64_t ctimeNs{};
  };

  struct InputFile {
    const char* path;  // NUL-terminated, stored in _sniRoutes.charStorage
    FileStamp stamp;   // taken before the file is read: a change racing with the read is seen, never missed
  };

  static FileStamp StampFile(const char* path) noexcept;

  void recordInputFiles(const TLSConfig& cfg);

  struct AlpnData {
    // private implementation detail (binary length-prefixed ALPN protocol list per RFC 7301)
    RawBytes32 wire;  // [len][bytes]...[len][bytes]
    // Incremented by the ALPN selection callback (server event loop), read by stats() from any thread.
    SingleWriterCounter<uint64_t> nbStrictMismatches;
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
  // Default OCSP response, stored in _sniRoutes.charStorage like the SNI ones: StapleOcspResponse() expects a
  // std::span<const std::byte> for both.
  std::span<const std::byte> _ocspResponse;
  RevocationData _revocationData;
  std::unique_ptr<KeyLogWriter> _keyLogWriter;
  std::shared_ptr<TlsTicketKeyStore> _ticketKeyStore;
  SniRoutes _sniRoutes;
  // Files read to build this context, see changedInputFile().
  vector<InputFile> _inputFiles;
  CtxPtr _ctx;
};

}  // namespace aeronet
