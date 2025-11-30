#include "aeronet/tls-context.hpp"

#include <openssl/evp.h>       // EVP_PKEY_free
#include <openssl/pem.h>       // PEM_read_bio_X509, PEM_read_bio_PrivateKey
#include <openssl/prov_ssl.h>  // TLS1_2_VERSION
#include <openssl/ssl.h>       // SSL_*, SSL_read/write, handshake, shutdown, SSL_CTX, SSL_get_error
#include <openssl/tls1.h>      // TLS1_2_VERSION (for OpenSSL < 3.0, safe to include anyway)
#include <openssl/types.h>     // SSL, SSL_CTX forward declarations
#include <openssl/x509.h>      // X509_free, X509_get_subject_name, X509_NAME_oneline
#include <openssl/x509_vfy.h>  // X509_STORE_add_cert

#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-raii.hpp"
#include "aeronet/tls-ticket-key-store.hpp"
#include "aeronet/toupperlower.hpp"

namespace aeronet {
namespace {
void ApplyCipherPolicy(SSL_CTX* ctx, const TLSConfig& cfg);
void AttachTicketStore(SSL_CTX* ctx, const std::shared_ptr<TlsTicketKeyStore>& store);

int parseTlsVersion(TLSConfig::Version ver) {
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

std::string NormalizeServerName(std::string_view host) {
  std::string normalized(host);
  for (char& ch : normalized) {
    ch = tolower(ch);
  }
  return normalized;
}

bool MatchesSniPattern(std::string_view pattern, bool wildcard, std::string_view serverName) {
  if (!wildcard) {
    return serverName == pattern;
  }
  if (serverName.size() <= pattern.size() - 2) {
    return false;
  }
  const std::string_view suffix = pattern.substr(1);  // drop '*'
  return serverName.ends_with(suffix.substr(1));
}

void LoadCertificateAndKey(SSL_CTX* ctx, std::string_view certPem, std::string_view keyPem, const char* certFilePath,
                           const char* keyFilePath) {
  if (!certPem.empty() && !keyPem.empty()) {
    auto certBio = makeMemBio(certPem.data(), static_cast<int>(certPem.size()));
    auto keyBio = makeMemBio(keyPem.data(), static_cast<int>(keyPem.size()));
    if (!certBio || !keyBio) {
      throw std::runtime_error("Failed to allocate BIO for in-memory cert/key");
    }
    X509Ptr certX509(::PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr), ::X509_free);
    PKeyPtr pkey(::PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr), ::EVP_PKEY_free);
    if (!certX509 || !pkey) {
      throw std::runtime_error("Failed to parse in-memory certificate/key");
    }
    if (::SSL_CTX_use_certificate(ctx, certX509.get()) != 1) {
      throw std::runtime_error("Failed to use in-memory certificate");
    }
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

  if (::SSL_CTX_check_private_key(ctx) != 1) {
    throw std::runtime_error("Private key check failed");
  }
}

void ConfigureContextOptions(SSL_CTX* ctx, const TLSConfig& cfg) {
  if (cfg.disableCompression) {
    ::SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);
  }
#if defined(AERONET_ENABLE_KTLS) && defined(SSL_OP_ENABLE_KTLS)
  if (cfg.ktlsMode != TLSConfig::KtlsMode::Disabled) {
    ::SSL_CTX_set_options(ctx, SSL_OP_ENABLE_KTLS);
  }
#endif
  if (cfg.cipherPolicy != TLSConfig::CipherPolicy::Default) {
    ApplyCipherPolicy(ctx, cfg);
  } else if (!cfg.cipherList().empty()) {
    if (::SSL_CTX_set_cipher_list(ctx, cfg.cipherListCstrView().c_str()) != 1) {
      throw std::runtime_error("Failed to set cipher list");
    }
  }
}

void ConfigureProtocolBounds(SSL_CTX* ctx, const TLSConfig& cfg) {
  if (cfg.minVersion != TLSConfig::Version{}) {
    int mv = parseTlsVersion(cfg.minVersion);
    if (mv == 0 || ::SSL_CTX_set_min_proto_version(ctx, mv) != 1) {
      throw std::runtime_error("Failed to set minimum TLS version");
    }
  }
  if (cfg.maxVersion != TLSConfig::Version{}) {
    int Mv = parseTlsVersion(cfg.maxVersion);
    if (Mv == 0 || ::SSL_CTX_set_max_proto_version(ctx, Mv) != 1) {
      throw std::runtime_error("Failed to set maximum TLS version");
    }
  }
}

void ConfigureClientVerification(SSL_CTX* ctx, const TLSConfig& cfg) {
  if (!cfg.requestClientCert) {
    return;
  }
  int verifyMode = SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE;
  if (cfg.requireClientCert) {
    verifyMode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
  }
  ::SSL_CTX_set_verify(ctx, verifyMode, nullptr);
  for (std::string_view pem : cfg.trustedClientCertsPem()) {
    if (pem.empty()) {
      throw std::runtime_error("Empty trusted client certificate PEM provided");
    }
    auto cbio = makeMemBio(pem.data(), static_cast<int>(pem.size()));
    if (!cbio) {
      throw std::runtime_error("Failed to alloc BIO for client trust cert");
    }
    X509Ptr cx(::PEM_read_bio_X509(cbio.get(), nullptr, nullptr, nullptr), ::X509_free);
    if (!cx) {
      throw std::runtime_error("Failed to parse trusted client certificate");
    }
    if (::SSL_CTX_get_cert_store(ctx) == nullptr) {
      throw std::runtime_error("No cert store available in SSL_CTX");
    }
    if (::X509_STORE_add_cert(::SSL_CTX_get_cert_store(ctx), cx.get()) != 1) {
      throw std::runtime_error("Failed to add trusted client certificate to store");
    }
  }
}

void ConfigureSessionTickets(SSL_CTX* ctx, const TLSConfig& cfg,
                             const std::shared_ptr<TlsTicketKeyStore>& ticketStore) {
  if (!cfg.sessionTickets.enabled) {
    ::SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
    return;
  }
  ::SSL_CTX_clear_options(ctx, SSL_OP_NO_TICKET);
  if (ticketStore) {
    AttachTicketStore(ctx, ticketStore);
  }
}

const char* CipherPolicyTls13(TLSConfig::CipherPolicy policy) {
  switch (policy) {
    case TLSConfig::CipherPolicy::Modern:
    case TLSConfig::CipherPolicy::Compatibility:
      return "TLS_AES_256_GCM_SHA384:TLS_AES_128_GCM_SHA256:TLS_CHACHA20_POLY1305_SHA256";
    case TLSConfig::CipherPolicy::Legacy:
      return "TLS_AES_256_GCM_SHA384:TLS_AES_128_GCM_SHA256";
    default:
      return nullptr;
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
      return nullptr;
  }
}

void ApplyCipherPolicy(SSL_CTX* ctx, const TLSConfig& cfg) {
  if (cfg.cipherPolicy == TLSConfig::CipherPolicy::Default) {
    return;
  }
  if (const char* suites13 = CipherPolicyTls13(cfg.cipherPolicy)) {
    if (::SSL_CTX_set_ciphersuites(ctx, suites13) != 1) {
      throw std::runtime_error("Failed to set TLS 1.3 cipher suites");
    }
  }
  if (const char* suites12 = CipherPolicyTls12(cfg.cipherPolicy)) {
    if (::SSL_CTX_set_cipher_list(ctx, suites12) != 1) {
      throw std::runtime_error("Failed to set TLS cipher list");
    }
  }
}

int TicketStoreIndex() {
  static std::once_flag flag;
  static int index = 0;
  std::call_once(flag, [] { index = ::SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr); });
  return index;
}

TlsTicketKeyStore* GetTicketStore(SSL_CTX* ctx) {
  if (ctx == nullptr) {
    return nullptr;
  }
  return static_cast<TlsTicketKeyStore*>(::SSL_CTX_get_ex_data(ctx, TicketStoreIndex()));
}

int SessionTicketCallback(SSL* ssl, unsigned char* keyName, unsigned char* iv, EVP_CIPHER_CTX* cctx, EVP_MAC_CTX* mctx,
                          int enc) {
  SSL_CTX* sslCtx = ::SSL_get_SSL_CTX(ssl);
  auto* storePtr = GetTicketStore(sslCtx);
  if (storePtr == nullptr) {
    return 0;
  }
  return storePtr->processTicket(keyName, iv, EVP_MAX_IV_LENGTH, cctx, mctx, enc);
}

void AttachTicketStore(SSL_CTX* ctx, const std::shared_ptr<TlsTicketKeyStore>& store) {
  if (ctx == nullptr || store == nullptr) {
    return;
  }
  ::SSL_CTX_set_ex_data(ctx, TicketStoreIndex(), store.get());
  ::SSL_CTX_set_tlsext_ticket_key_evp_cb(ctx, &SessionTicketCallback);
}
}  // namespace

void TlsContext::CtxDel::operator()(ssl_ctx_st* ctxPtr) const noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  ::SSL_CTX_free(reinterpret_cast<SSL_CTX*>(ctxPtr));
}

TlsContext::~TlsContext() = default;

TlsContext::TlsContext(const TLSConfig& cfg, TlsMetricsExternal* metrics,
                       std::shared_ptr<TlsTicketKeyStore> ticketKeyStore)
    : _ctx(::SSL_CTX_new(TLS_server_method())), _ticketKeyStore(std::move(ticketKeyStore)) {
  if (!_ctx) {
    throw std::runtime_error("SSL_CTX_new failed");
  }
  auto* raw = reinterpret_cast<SSL_CTX*>(_ctx.get());
  ConfigureContextOptions(raw, cfg);
  ConfigureProtocolBounds(raw, cfg);
  std::string_view certPem = cfg.certPem();
  std::string_view keyPem = cfg.keyPem();
  if (!certPem.empty() && !keyPem.empty()) {
    auto certBio = makeMemBio(certPem.data(), static_cast<int>(certPem.size()));
    auto keyBio = makeMemBio(keyPem.data(), static_cast<int>(keyPem.size()));
    if (!certBio || !keyBio) {
      throw std::runtime_error("Failed to allocate BIO for in-memory cert/key");
    }
    X509Ptr certX509(::PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr), ::X509_free);
    PKeyPtr pkey(::PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr), ::EVP_PKEY_free);
    if (!certX509 || !pkey) {
      throw std::runtime_error("Failed to parse in-memory certificate/key");
    }
    if (::SSL_CTX_use_certificate(raw, certX509.get()) != 1) {
      throw std::runtime_error("Failed to use in-memory certificate");
    }
    if (::SSL_CTX_use_PrivateKey(raw, pkey.get()) != 1) {
      throw std::runtime_error("Failed to use in-memory private key");
    }
  } else {
    if (::SSL_CTX_use_certificate_file(raw, cfg.certFileCstrView().c_str(), SSL_FILETYPE_PEM) != 1) {
      throw std::runtime_error("Failed to load certificate");
    }
    if (::SSL_CTX_use_PrivateKey_file(raw, cfg.keyFileCstrView().c_str(), SSL_FILETYPE_PEM) != 1) {
      throw std::runtime_error("Failed to load private key");
    }
  }
  if (::SSL_CTX_check_private_key(raw) != 1) {
    throw std::runtime_error("Private key check failed");
  }
  ConfigureClientVerification(raw, cfg);

  auto alpnProtocols = cfg.alpnProtocols();
  std::size_t wireLen = std::accumulate(alpnProtocols.begin(), alpnProtocols.end(), std::size_t{0},
                                        [](std::size_t sum, const auto& proto) { return sum + 1UL + proto.size(); });
  if (wireLen != 0) {
    _alpnData = std::make_unique<AlpnData>(RawBytes{wireLen}, cfg.alpnMustMatch, metrics);
    for (const auto& proto : alpnProtocols) {
      _alpnData->wire.unchecked_push_back(static_cast<std::byte>(proto.size()));
      _alpnData->wire.unchecked_append(reinterpret_cast<const std::byte*>(proto.data()), proto.size());
    }
    ::SSL_CTX_set_alpn_select_cb(raw, &TlsContext::SelectAlpn, _alpnData.get());
  }

  if (cfg.sessionTickets.enabled) {
    if (!_ticketKeyStore) {
      _ticketKeyStore = std::make_shared<TlsTicketKeyStore>(cfg.sessionTickets.lifetime, cfg.sessionTickets.maxKeys);
    }
    if (!cfg.sessionTicketKeys().empty()) {
      _ticketKeyStore->loadStaticKeys(cfg.sessionTicketKeys());
    }
  }
  ConfigureSessionTickets(raw, cfg, _ticketKeyStore);

  const auto& sniCerts = cfg.sniCertificates();
  if (!sniCerts.empty()) {
    _sniRoutes = std::make_unique<SniRoutes>(std::make_unique<SniRoute[]>(sniCerts.size()), sniCerts.size());
    SniRoute* pRoute = _sniRoutes->routes.get();
    for (const auto& entry : sniCerts) {
      CtxPtr routeCtx{reinterpret_cast<ssl_ctx_st*>(::SSL_CTX_new(TLS_server_method()))};
      if (!routeCtx) {
        throw std::runtime_error("SSL_CTX_new failed for SNI certificate");
      }
      auto* routeRaw = reinterpret_cast<SSL_CTX*>(routeCtx.get());
      ConfigureContextOptions(routeRaw, cfg);
      ConfigureProtocolBounds(routeRaw, cfg);
      if (entry.certPem().empty()) {
        LoadCertificateAndKey(routeRaw, std::string_view{}, std::string_view{}, entry.certFileCstrView().c_str(),
                              entry.keyFileCstrView().c_str());
      } else {
        LoadCertificateAndKey(routeRaw, entry.certPem(), entry.keyPem(), nullptr, nullptr);
      }
      ConfigureClientVerification(routeRaw, cfg);
      if (_alpnData != nullptr) {
        ::SSL_CTX_set_alpn_select_cb(routeRaw, &TlsContext::SelectAlpn, _alpnData.get());
      }
      ConfigureSessionTickets(routeRaw, cfg, _ticketKeyStore);
      *pRoute = SniRoute(RawChars(entry.pattern()), entry.isWildcard, std::move(routeCtx));
      ++pRoute;
    }
    ::SSL_CTX_set_tlsext_servername_arg(raw, _sniRoutes.get());
    ::SSL_CTX_set_tlsext_servername_callback(raw, &TlsContext::SelectSniRoute);
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
    if (data->metrics != nullptr) {
      ++data->metrics->alpnStrictMismatches;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
  }
  return SSL_TLSEXT_ERR_NOACK;
}

int TlsContext::SelectSniRoute(SSL* ssl, int* alert, void* arg) {
  auto& routes = *reinterpret_cast<SniRoutes*>(arg);
  std::span<SniRoute> routeSpan(routes.routes.get(), routes.nbRoutes);
  assert(arg != nullptr && !routeSpan.empty());
  const char* serverName = ::SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  if (serverName == nullptr) {
    return SSL_TLSEXT_ERR_NOACK;
  }
  std::string normalized = NormalizeServerName(serverName);
  for (auto& route : routeSpan) {
    if (MatchesSniPattern(route.pattern, route.wildcard, normalized)) {
      auto* nextCtx = reinterpret_cast<SSL_CTX*>(route.ctx.get());
      if (nextCtx == nullptr) {
        break;
      }
      if (::SSL_set_SSL_CTX(ssl, nextCtx) != nullptr) {
        return SSL_TLSEXT_ERR_OK;
      }
      if (alert != nullptr) {
        *alert = SSL_AD_INTERNAL_ERROR;
      }
      return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
  }
  return SSL_TLSEXT_ERR_NOACK;
}

}  // namespace aeronet