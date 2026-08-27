#include "aeronet/tls-context.hpp"

#include <openssl/err.h>
#include <openssl/evp.h>  // EVP_PKEY_free
#include <openssl/ocsp.h>
#include <openssl/pem.h>       // PEM_read_bio_X509, PEM_read_bio_PrivateKey
#include <openssl/prov_ssl.h>  // TLS1_2_VERSION
#include <openssl/ssl.h>       // SSL_*, SSL_read/write, handshake, shutdown, SSL_CTX, SSL_get_error
#include <openssl/tls1.h>      // TLS1_2_VERSION (for OpenSSL < 3.0, safe to include anyway)
#include <openssl/types.h>     // SSL, SSL_CTX forward declarations
#include <openssl/x509_vfy.h>  // X509_STORE_add_cert

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#ifdef AERONET_POSIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "aeronet/log-noexcept.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-handshake-observer.hpp"
#include "aeronet/tls-raii.hpp"
#include "aeronet/tls-ticket-key-store.hpp"

namespace aeronet {

namespace {

constexpr std::size_t kMaxOcspResponseBytes = 1U << 20U;

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const int kTicketStoreIndex = ::SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const int kRevocationDataIndex = ::SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);

// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const int kKeyLogWriterIndex = ::SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);

void ApplyCipherPolicy(SSL_CTX* ctx, const TLSConfig& cfg);

int ParseTlsVersion(TLSConfig::Version ver) {
  if (ver == TLSConfig::TLS_1_2) {
    return TLS1_2_VERSION;
  }
#ifdef TLS1_3_VERSION
  if (ver == TLSConfig::TLS_1_3) {
    return TLS1_3_VERSION;
  }
#endif
  return 0;  // unknown / not set
}

bool MatchesSniPattern(std::string_view pattern, bool wildcard, std::string_view serverName) {
  if (!wildcard) {
    return CaseInsensitiveEqual(serverName, pattern);
  }
  // Keep the dot from "*." in the suffix. This enforces a label boundary and prevents
  // "*.example.com" from matching "notexample.com".
  pattern.remove_prefix(1);
  if (serverName.size() <= pattern.size()) {
    return false;
  }
  const std::size_t prefixSize = serverName.size() - pattern.size();
  return !serverName.substr(0, prefixSize).contains('.') &&
         CaseInsensitiveEqual(serverName.substr(prefixSize), pattern);
}

void LoadCertificateAndKey(SSL_CTX* ctx, std::string_view certPem, std::string_view keyPem, const char* certFilePath,
                           const char* keyFilePath) {
  if (!certPem.empty() && !keyPem.empty()) {
    auto certBio = MakeMemBio(certPem.data(), static_cast<int>(certPem.size()));
    auto keyBio = MakeMemBio(keyPem.data(), static_cast<int>(keyPem.size()));
    auto certX509 = MakeX509(::PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr));
    auto pkey = MakePKey(::PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr));

    // PEM_read_bio_X509 already validated the certificate; use_certificate should always succeed.
    [[maybe_unused]] int certRc = ::SSL_CTX_use_certificate(ctx, certX509.get());
    assert(certRc == 1 && "SSL_CTX_use_certificate failed with valid X509");
    if (::SSL_CTX_use_PrivateKey(ctx, pkey.get()) != 1) {
      throw std::runtime_error("Failed to use in-memory private key");
    }
  } else {
    if (certFilePath == nullptr || keyFilePath == nullptr) {
      throw std::runtime_error("Certificate or key file path missing");
    }
    if (::SSL_CTX_use_certificate_file(ctx, certFilePath, SSL_FILETYPE_PEM) != 1) {
      throw std::runtime_error("Failed to load certificate");
    }
    if (::SSL_CTX_use_PrivateKey_file(ctx, keyFilePath, SSL_FILETYPE_PEM) != 1) {
      throw std::runtime_error("Failed to load private key");
    }
  }

  // SSL_CTX_use_PrivateKey already validates the key against the loaded certificate.
  [[maybe_unused]] const int keyCheckRc = ::SSL_CTX_check_private_key(ctx);
  assert(keyCheckRc == 1 && "private key check failed despite use_PrivateKey success");
}

void ConfigureContextOptions(SSL_CTX* ctx, const TLSConfig& cfg) {
  ::SSL_CTX_set_options(ctx, SSL_OP_CIPHER_SERVER_PREFERENCE);
  assert((::SSL_CTX_get_options(ctx) & SSL_OP_CIPHER_SERVER_PREFERENCE) != 0);
#ifdef SSL_OP_NO_RENEGOTIATION
  ::SSL_CTX_set_options(ctx, SSL_OP_NO_RENEGOTIATION);
  assert((::SSL_CTX_get_options(ctx) & SSL_OP_NO_RENEGOTIATION) != 0);
#endif
  if (cfg.disableCompression) {
    ::SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
    assert((::SSL_CTX_get_options(ctx) & SSL_OP_NO_COMPRESSION) != 0);
  } else {
    ::SSL_CTX_clear_options(ctx, SSL_OP_NO_COMPRESSION);
    assert((::SSL_CTX_get_options(ctx) & SSL_OP_NO_COMPRESSION) == 0);
  }
  switch (cfg.ktlsMode) {
    case TLSConfig::KtlsMode::Disabled:
      ::SSL_CTX_clear_options(ctx, SSL_OP_ENABLE_KTLS);
      assert((::SSL_CTX_get_options(ctx) & SSL_OP_ENABLE_KTLS) == 0);
      break;
    case TLSConfig::KtlsMode::Opportunistic:
      [[fallthrough]];
    case TLSConfig::KtlsMode::Enabled:
      [[fallthrough]];
    case TLSConfig::KtlsMode::Required:
      // Required will be checked at handshake time, Enabled will log warnings if not available
      ::SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS);
      assert((::SSL_CTX_get_options(ctx) & SSL_OP_ENABLE_KTLS) != 0);
      break;
    default:
      throw std::invalid_argument("Invalid kTLS mode");
  }
  if (cfg.cipherPolicy != TLSConfig::CipherPolicy::Default) {
    ApplyCipherPolicy(ctx, cfg);
  } else if (!cfg.cipherList().empty() && ::SSL_CTX_set_cipher_list(ctx, cfg.cipherListCstr()) != 1) {
    throw std::runtime_error("Failed to set cipher list");
  }
}

void ConfigureProtocolBounds(SSL_CTX* ctx, const TLSConfig& cfg) {
  const int minVersion = cfg.minVersion == TLSConfig::Version{} ? TLS1_2_VERSION : ParseTlsVersion(cfg.minVersion);
  if (minVersion == 0 || ::SSL_CTX_set_min_proto_version(ctx, minVersion) != 1) {
    throw std::runtime_error("Failed to set minimum TLS version");
  }
  if (cfg.maxVersion != TLSConfig::Version{}) {
    const int Mv = ParseTlsVersion(cfg.maxVersion);
    if (Mv == 0 || ::SSL_CTX_set_max_proto_version(ctx, Mv) != 1) {
      throw std::runtime_error("Failed to set maximum TLS version");
    }
  }
}

void LoadCrl(SSL_CTX* ctx, const TLSConfig& cfg) {
  if (cfg.crlFile().empty()) {
    return;
  }
  X509_STORE* store = ::SSL_CTX_get_cert_store(ctx);
  assert(store != nullptr && "SSL_CTX missing cert store");
  X509_LOOKUP* lookup = ::X509_STORE_add_lookup(store, ::X509_LOOKUP_file());
  if (lookup == nullptr) {
    throw std::runtime_error("Failed to create TLS CRL lookup for: " + std::string(cfg.crlFile()));
  }
  int loaded = ::X509_load_crl_file(lookup, cfg.crlFileCstr(), X509_FILETYPE_PEM);
  if (loaded == 0) {
    ::ERR_clear_error();
    loaded = ::X509_load_crl_file(lookup, cfg.crlFileCstr(), X509_FILETYPE_ASN1);
  }
  if (loaded == 0) {
    ::ERR_clear_error();
    throw std::runtime_error("Failed to load TLS CRL file: " + std::string(cfg.crlFile()));
  }
  unsigned long flags = X509_V_FLAG_CRL_CHECK;
  if (cfg.crlCheckAll) {
    flags |= X509_V_FLAG_CRL_CHECK_ALL;
  }
  if (::X509_STORE_set_flags(store, flags) != 1) {
    throw std::runtime_error("Failed to enable TLS CRL verification for: " + std::string(cfg.crlFile()));
  }
}

void ConfigureClientVerification(SSL_CTX* ctx, const TLSConfig& cfg, void* revocationData,
                                 int (*verifyCallback)(int, X509_STORE_CTX*)) {
  // requireClientCert implies requestClientCert (normally normalized by TLSConfig::validate). Treat
  // requireClientCert as sufficient here too, so a TlsContext built from a config that bypassed
  // validation still enforces mTLS instead of silently accepting certificate-less clients.
  if (!cfg.requestClientCert && !cfg.requireClientCert) {
    if (!cfg.crlFile().empty() || cfg.revocationCallback != nullptr) {
      throw std::invalid_argument("TLS revocation checks require client certificate verification");
    }
    return;
  }
  int verifyMode = SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE;
  if (cfg.requireClientCert) {
    // NOLINTNEXTLINE(bugprone-signed-bitwise)
    verifyMode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
  }
  if (cfg.revocationCallback != nullptr && ::SSL_CTX_set_ex_data(ctx, kRevocationDataIndex, revocationData) != 1) {
    throw std::runtime_error("Failed to attach the TLS revocation callback context");
  }
  ::SSL_CTX_set_verify(ctx, verifyMode, cfg.revocationCallback == nullptr ? nullptr : verifyCallback);
  for (std::string_view pem : cfg.trustedClientCertsPem()) {
    if (pem.empty()) {
      throw std::runtime_error("Empty trusted client certificate PEM provided");
    }
    auto cbio = MakeMemBio(pem.data(), static_cast<int>(pem.size()));
    auto cx = MakeX509(::PEM_read_bio_X509(cbio.get(), nullptr, nullptr, nullptr));
    // A freshly created SSL_CTX always has a cert store.
    assert(::SSL_CTX_get_cert_store(ctx) != nullptr && "SSL_CTX missing cert store");
    if (::X509_STORE_add_cert(::SSL_CTX_get_cert_store(ctx), cx.get()) != 1) {
      throw std::runtime_error("Failed to add trusted client certificate to store");
    }
  }
  LoadCrl(ctx, cfg);
}

std::span<const std::byte> LoadOcspResponse(ObjectArrayPool<char>& charStorage, std::string_view path) {
  if (path.empty()) {
    return {};
  }
  std::ifstream file(std::string(path), std::ios::binary | std::ios::ate);
  if (!file) {
    throw std::runtime_error("Failed to open TLS OCSP response file: " + std::string(path));
  }
  const std::streampos endPos = file.tellg();
  if (endPos <= 0 || endPos > static_cast<std::streamoff>(kMaxOcspResponseBytes)) {
    throw std::runtime_error("TLS OCSP response file is empty or exceeds 1 MiB: " + std::string(path));
  }
  const auto size = static_cast<std::size_t>(endPos);
  char* pBuf = charStorage.allocateAndDefaultConstruct(size);
  file.seekg(0);
  if (!file.read(pBuf, static_cast<std::streamsize>(size))) {
    throw std::runtime_error("Failed to read TLS OCSP response file: " + std::string(path));
  }
  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(pBuf);
  const unsigned char* const end = cursor + size;
  std::unique_ptr<OCSP_RESPONSE, decltype(&::OCSP_RESPONSE_free)> parsed(
      ::d2i_OCSP_RESPONSE(nullptr, &cursor, static_cast<long>(size)), &::OCSP_RESPONSE_free);
  if (!parsed || cursor != end || ::OCSP_response_status(parsed.get()) != OCSP_RESPONSE_STATUS_SUCCESSFUL) {
    ::ERR_clear_error();
    throw std::runtime_error("TLS OCSP file does not contain one successful DER response: " + std::string(path));
  }
  return {reinterpret_cast<const std::byte*>(pBuf), size};
}

int SessionTicketCallback(SSL* ssl, unsigned char* keyName, unsigned char* iv, EVP_CIPHER_CTX* cctx, EVP_MAC_CTX* mctx,
                          int enc) {
  SSL_CTX* sslCtx = ::SSL_get_SSL_CTX(ssl);
  TlsTicketKeyStore* storePtr = static_cast<TlsTicketKeyStore*>(::SSL_CTX_get_ex_data(sslCtx, kTicketStoreIndex));
  // The callback is only registered when session tickets are enabled, which always sets the ex_data.
  assert(storePtr != nullptr && "SessionTicketCallback called with null ticket key store");
  return storePtr->processTicket(keyName, iv, EVP_MAX_IV_LENGTH, cctx, mctx, enc);
}

void ConfigureSessionTickets(SSL_CTX* ctx, const TLSConfig& cfg,
                             const std::shared_ptr<TlsTicketKeyStore>& ticketStore) {
  if (!cfg.sessionTickets.enabled) {
    [[maybe_unused]] const auto ticketOpts = ::SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
    assert((ticketOpts & SSL_OP_NO_TICKET) != 0 && "SSL_CTX_set_options failed to set SSL_OP_NO_TICKET");
    return;
  }
  [[maybe_unused]] const auto clearOpts = ::SSL_CTX_clear_options(ctx, SSL_OP_NO_TICKET);
  assert((clearOpts & SSL_OP_NO_TICKET) == 0 && "SSL_CTX_clear_options failed to clear SSL_OP_NO_TICKET");
  assert(ticketStore != nullptr);
  ::SSL_CTX_set_ex_data(ctx, kTicketStoreIndex, ticketStore.get());
  ::SSL_CTX_set_tlsext_ticket_key_evp_cb(ctx, &SessionTicketCallback);
}

const char* CipherPolicyTls13(TLSConfig::CipherPolicy policy) {
  switch (policy) {
    case TLSConfig::CipherPolicy::Modern:
      [[fallthrough]];
    case TLSConfig::CipherPolicy::Compatibility:
      return "TLS_AES_256_GCM_SHA384:TLS_AES_128_GCM_SHA256:TLS_CHACHA20_POLY1305_SHA256";
    case TLSConfig::CipherPolicy::Legacy:
      return "TLS_AES_256_GCM_SHA384:TLS_AES_128_GCM_SHA256";
    default:
      throw std::invalid_argument("Invalid cipher policy");
  }
}

const char* CipherPolicyTls12(TLSConfig::CipherPolicy policy) {
  switch (policy) {
    case TLSConfig::CipherPolicy::Modern:
      return "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
             "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
             "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256";
    case TLSConfig::CipherPolicy::Compatibility:
      return "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
             "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
             "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
             "ECDHE-RSA-AES256-SHA384:ECDHE-RSA-AES128-SHA256";
    case TLSConfig::CipherPolicy::Legacy:
      return "ECDHE-RSA-AES256-SHA:ECDHE-RSA-AES128-SHA:AES256-SHA:AES128-SHA";
    default:
      // CipherPolicyTls13 throws first for invalid policies, so this is unreachable.
      std::unreachable();
  }
}

void ApplyCipherPolicy(SSL_CTX* ctx, const TLSConfig& cfg) {
  // Hardcoded valid cipher strings - these calls should always succeed.
  [[maybe_unused]] const int suiteRc = ::SSL_CTX_set_ciphersuites(ctx, CipherPolicyTls13(cfg.cipherPolicy));
  assert(suiteRc == 1 && "Failed to set TLS 1.3 cipher suites");
  [[maybe_unused]] const int listRc = ::SSL_CTX_set_cipher_list(ctx, CipherPolicyTls12(cfg.cipherPolicy));
  assert(listRc == 1 && "Failed to set TLS cipher list");
}

}  // namespace

class TlsContext::KeyLogWriter {
 public:
  explicit KeyLogWriter(std::string_view path) {
    std::string ownedPath(path);
#ifdef AERONET_POSIX
    const int fd = ::open(ownedPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd >= 0) {
      _file = ::fdopen(fd, "ab");
      if (_file == nullptr) {
        ::close(fd);
      }
    }
#else
    _file = std::fopen(ownedPath.c_str(), "ab");
#endif
    if (_file == nullptr) {
      throw std::runtime_error("Failed to open TLS key log file: " + ownedPath);
    }
    std::setvbuf(_file, nullptr, _IONBF, 0);
  }

  KeyLogWriter(const KeyLogWriter&) = delete;
  KeyLogWriter& operator=(const KeyLogWriter&) = delete;

  ~KeyLogWriter() {
    if (_file != nullptr && std::fclose(_file) != 0) {
      log_noexcept::error("Failed to close TLS key log file");
    }
  }

  void write(std::string_view line) {
    std::scoped_lock lock(_mutex);
    if (std::fwrite(line.data(), line.size(), 1, _file) != 1 || std::fputc('\n', _file) == EOF) {
      log::error("Failed to append TLS key log line");
    }
  }

 private:
  std::FILE* _file{nullptr};
  std::mutex _mutex;
};

void TlsContext::CtxDel::operator()(ssl_ctx_st* ctxPtr) const noexcept {
  ::SSL_CTX_free(reinterpret_cast<SSL_CTX*>(ctxPtr));
}

TlsContext::~TlsContext() {
  // Destroy every SSL_CTX while callback arguments owned by this object are still alive.
  _ctx.reset();
  _sniRoutes.routes.reset();
  _sniRoutes.nbRoutes = 0;
}

TlsContext::TlsContext(const TLSConfig& cfg, std::shared_ptr<TlsTicketKeyStore> ticketKeyStore)
    : _revocationData{cfg.revocationCallback, cfg.revocationUserContext},
      _ticketKeyStore(std::move(ticketKeyStore)),
      _ctx(::SSL_CTX_new(TLS_server_method())) {
  if (!_ctx) {
    throw std::bad_alloc();
  }

  auto* ctx = reinterpret_cast<SSL_CTX*>(_ctx.get());
  ConfigureContextOptions(ctx, cfg);
  ConfigureProtocolBounds(ctx, cfg);
  LoadCertificateAndKey(ctx, cfg.certPem(), cfg.keyPem(), cfg.certFileCstr(), cfg.keyFileCstr());
  ConfigureClientVerification(ctx, cfg, &_revocationData, &TlsContext::VerifyPeerCertificate);

  auto ocspResponse = LoadOcspResponse(_sniRoutes.charStorage, cfg.ocspResponseFile());
  if (!ocspResponse.empty()) {
    _ocspResponse.assign(ocspResponse);
    _sniRoutes.charStorage.shrinkLastAllocated(reinterpret_cast<const char*>(ocspResponse.data()), 0);

    if (::SSL_CTX_set_tlsext_status_arg(ctx, &_ocspResponse) != 1 ||
        ::SSL_CTX_set_tlsext_status_cb(ctx, &TlsContext::StapleOcspResponse) != 1) {
      throw std::runtime_error("Failed to configure the default TLS OCSP staple");
    }
  }

  if (!cfg.keyLogFile().empty()) {
#ifdef NDEBUG
    throw std::invalid_argument("TLS key logging is available only in debug builds");
#else
    _keyLogWriter = std::make_unique<KeyLogWriter>(cfg.keyLogFile());
    if (::SSL_CTX_set_ex_data(ctx, kKeyLogWriterIndex, _keyLogWriter.get()) != 1) {
      throw std::runtime_error("Failed to attach the TLS key log writer");
    }
    ::SSL_CTX_set_keylog_callback(ctx, &TlsContext::LogSessionKeys);
#endif
  }

  const auto alpnProtocols = cfg.alpnProtocols();
  const std::size_t wireLen = std::ranges::fold_left(
      alpnProtocols, std::size_t{0}, [](std::size_t sum, const auto& proto) { return sum + 1UL + proto.size(); });
  if (wireLen != 0) {
    _alpnData = AlpnData{RawBytes32{wireLen}, 0, cfg.alpnMustMatch};
    for (const auto& proto : alpnProtocols) {
      _alpnData.wire.unchecked_push_back(static_cast<std::byte>(proto.size()));
      _alpnData.wire.unchecked_append(reinterpret_cast<const std::byte*>(proto.data()), proto.size());
    }
    ::SSL_CTX_set_alpn_select_cb(ctx, &TlsContext::SelectAlpn, &_alpnData);
  }

  if (cfg.sessionTickets.enabled) {
    if (!_ticketKeyStore) {
      _ticketKeyStore = std::make_shared<TlsTicketKeyStore>(cfg.sessionTickets.lifetime, cfg.sessionTickets.maxKeys);
    }
    if (!cfg.sessionTicketKeys().empty()) {
      _ticketKeyStore->loadStaticKeys(cfg.sessionTicketKeys());
    }
  }
  ConfigureSessionTickets(ctx, cfg, _ticketKeyStore);

  const auto& sniCerts = cfg.sniCertificates();
  if (!sniCerts.empty()) {
    _sniRoutes = SniRoutes{std::make_unique<SniRoute[]>(sniCerts.size()), sniCerts.size(), ObjectArrayPool<char>{}};
    SniRoute* pRoute = _sniRoutes.routes.get();
    for (const auto& entry : sniCerts) {
      CtxPtr routeCtx{reinterpret_cast<ssl_ctx_st*>(::SSL_CTX_new(::TLS_server_method()))};
      if (!routeCtx) {
        throw std::bad_alloc();
      }
      auto* routeRaw = reinterpret_cast<SSL_CTX*>(routeCtx.get());
      ConfigureContextOptions(routeRaw, cfg);
      ConfigureProtocolBounds(routeRaw, cfg);
      if (entry.certPem().empty()) {
        LoadCertificateAndKey(routeRaw, std::string_view{}, std::string_view{}, entry.certFileCstr(),
                              entry.keyFileCstr());
      } else {
        LoadCertificateAndKey(routeRaw, entry.certPem(), entry.keyPem(), nullptr, nullptr);
      }
      ConfigureClientVerification(routeRaw, cfg, &_revocationData, &TlsContext::VerifyPeerCertificate);
      if (wireLen != 0) {
        ::SSL_CTX_set_alpn_select_cb(routeRaw, &TlsContext::SelectAlpn, &_alpnData);
      }
      ConfigureSessionTickets(routeRaw, cfg, _ticketKeyStore);

      char* pPattern = _sniRoutes.charStorage.allocateAndDefaultConstruct(entry.pattern().size());
      Copy(entry.pattern(), pPattern);

      ocspResponse = LoadOcspResponse(_sniRoutes.charStorage, entry.ocspResponseFile());

      *pRoute = SniRoute(std::string_view(pPattern, entry.pattern().size()), entry.isWildcard, std::move(routeCtx),
                         ocspResponse);
      routeRaw = reinterpret_cast<SSL_CTX*>(pRoute->ctx.get());
      if (!pRoute->ocspResponse.empty() &&
          (::SSL_CTX_set_tlsext_status_arg(routeRaw, &pRoute->ocspResponse) != 1 ||
           ::SSL_CTX_set_tlsext_status_cb(routeRaw, &TlsContext::StapleOcspResponse) != 1)) {
        throw std::runtime_error("Failed to configure an SNI TLS OCSP staple for: " + std::string(entry.pattern()));
      }
      if (_keyLogWriter) {
        if (::SSL_CTX_set_ex_data(routeRaw, kKeyLogWriterIndex, _keyLogWriter.get()) != 1) {
          throw std::runtime_error("Failed to attach the SNI TLS key log writer for: " + std::string(entry.pattern()));
        }
        ::SSL_CTX_set_keylog_callback(routeRaw, &TlsContext::LogSessionKeys);
      }
      ++pRoute;
    }
    ::SSL_CTX_set_tlsext_servername_arg(ctx, &_sniRoutes);
    ::SSL_CTX_set_tlsext_servername_callback(ctx, &TlsContext::SelectSniRoute);
  }

  const auto opts = ::SSL_CTX_get_options(ctx);
  const bool ktlsAllowed = (opts & SSL_OP_ENABLE_KTLS) != 0;
  const bool compressionAllowed = (opts & SSL_OP_NO_COMPRESSION) == 0;

  log::debug("SSL_CTX options:");
  log::debug(" - kTLS:        {}", ktlsAllowed ? "enabled" : "disabled");
  log::debug(" - compression: {}", compressionAllowed ? "enabled" : "disabled");
}

int TlsContext::StapleOcspResponse(SSL* ssl, void* arg) {
  const auto& response = *static_cast<const RawBytes32*>(arg);
  assert(!response.empty());
  auto* copy =
      static_cast<unsigned char*>(::OPENSSL_memdup(response.data(), static_cast<std::size_t>(response.size())));
  if (copy == nullptr) {
    return SSL_TLSEXT_ERR_ALERT_FATAL;
  }
  if (::SSL_set_tlsext_status_ocsp_resp(ssl, copy, static_cast<int>(response.size())) != 1) {
    ::OPENSSL_free(copy);
    return SSL_TLSEXT_ERR_ALERT_FATAL;
  }
  return SSL_TLSEXT_ERR_OK;
}

int TlsContext::VerifyPeerCertificate(int preverifyOk, X509_STORE_CTX* storeCtx) {
  if (preverifyOk != 1) {
    return 0;
  }
  auto* ssl = static_cast<SSL*>(::X509_STORE_CTX_get_ex_data(storeCtx, ::SSL_get_ex_data_X509_STORE_CTX_idx()));
  if (ssl == nullptr) {
    ::X509_STORE_CTX_set_error(storeCtx, X509_V_ERR_APPLICATION_VERIFICATION);
    return 0;
  }
  SSL_CTX* sslCtx = ::SSL_get_SSL_CTX(ssl);
  auto* data = static_cast<RevocationData*>(::SSL_CTX_get_ex_data(sslCtx, kRevocationDataIndex));
  if (data == nullptr || data->callback == nullptr) {
    return 1;
  }
  X509* certificate = ::X509_STORE_CTX_get_current_cert(storeCtx);
  const TlsRevocationStatus status = data->callback(
      TlsPeerCertificateView{certificate, ::X509_STORE_CTX_get_error_depth(storeCtx)}, data->userContext);
  switch (status) {
    case TlsRevocationStatus::NoOpinion:
      [[fallthrough]];
    case TlsRevocationStatus::Good:
      return 1;
    case TlsRevocationStatus::Revoked:
      ::X509_STORE_CTX_set_error(storeCtx, X509_V_ERR_CERT_REVOKED);
      return 0;
    default:
      ::X509_STORE_CTX_set_error(storeCtx, X509_V_ERR_APPLICATION_VERIFICATION);
      return 0;
  }
}

void TlsContext::LogSessionKeys(const SSL* ssl, const char* line) {
  SSL_CTX* sslCtx = ::SSL_get_SSL_CTX(ssl);
  auto* writer = static_cast<KeyLogWriter*>(::SSL_CTX_get_ex_data(sslCtx, kKeyLogWriterIndex));
  if (writer != nullptr && line != nullptr) {
    writer->write(line);
  }
}

int TlsContext::SelectAlpn([[maybe_unused]] SSL* ssl, const unsigned char** out, unsigned char* outlen,
                           const unsigned char* in, unsigned int inlen, void* arg) {
  auto* data = reinterpret_cast<AlpnData*>(arg);
  assert(data != nullptr && !data->wire.empty());
  const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data->wire.data());
  const unsigned int prefLen = static_cast<unsigned int>(data->wire.size());
  for (unsigned int prefIndex = 0; prefIndex < prefLen;) {
    const unsigned char* val = ptr + prefIndex + 1;
    const unsigned int len = ptr[prefIndex];
    for (unsigned int clientIndex = 0; clientIndex < inlen;) {
      unsigned int clen = in[clientIndex];
      const unsigned char* cval = in + clientIndex + 1;
      if (clen == len && std::memcmp(val, cval, len) == 0) {
        *out = val;
        *outlen = static_cast<unsigned char>(len);
        return SSL_TLSEXT_ERR_OK;
      }
      clientIndex += 1 + clen;
    }
    prefIndex += 1 + len;
  }
  if (data->mustMatch) {
    ++data->nbStrictMismatches;
    if (auto* obs = GetTlsHandshakeObserver(reinterpret_cast<ssl_st*>(ssl))) {
      obs->alpnStrictMismatch = true;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
  }
  return SSL_TLSEXT_ERR_NOACK;
}

int TlsContext::SelectSniRoute(SSL* ssl, [[maybe_unused]] int* alert, void* arg) {
  auto& routes = *reinterpret_cast<SniRoutes*>(arg);
  std::span<SniRoute> routeSpan(routes.routes.get(), routes.nbRoutes);
  assert(arg != nullptr && !routeSpan.empty());
  const char* serverName = ::SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (serverName == nullptr) {
    return SSL_TLSEXT_ERR_NOACK;
  }
  for (auto& route : routeSpan) {
    if (MatchesSniPattern(route.pattern, route.wildcard, serverName)) {
      auto* nextCtx = reinterpret_cast<SSL_CTX*>(route.ctx.get());
      assert(nextCtx != nullptr);
      // SSL_set_SSL_CTX should always succeed when both the SSL and CTX are valid.
      [[maybe_unused]] auto* rc = ::SSL_set_SSL_CTX(ssl, nextCtx);
      assert(rc != nullptr && "SSL_set_SSL_CTX failed during SNI routing");
      return SSL_TLSEXT_ERR_OK;
    }
  }
  return SSL_TLSEXT_ERR_NOACK;
}

}  // namespace aeronet
