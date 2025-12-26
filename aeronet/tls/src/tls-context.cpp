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
#include <new>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/raw-bytes.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/string-equal-ignore-case.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/tls-handshake-observer.hpp"
#include "aeronet/tls-raii.hpp"
#include "aeronet/tls-ticket-key-store.hpp"
#include "spdlog/common.h"
#include "spdlog/spdlog.h"

namespace aeronet {

namespace {

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
  // pattern starts with "*.", so require serverName to be longer than the remaining
  // pattern characters after the "*." prefix. The `- 2` strips the leading "*." from
  // `pattern` (two characters) when comparing lengths for a wildcard match.
  pattern = pattern.substr(2);
  if (serverName.size() <= pattern.size()) {
    return false;
  }
  serverName.remove_prefix(serverName.size() - pattern.size());
  return CaseInsensitiveEqual(serverName, pattern);
}

void LoadCertificateAndKey(SSL_CTX* ctx, std::string_view certPem, std::string_view keyPem, const char* certFilePath,
                           const char* keyFilePath) {
  if (!certPem.empty() && !keyPem.empty()) {
    auto certBio = MakeMemBio(certPem.data(), static_cast<int>(certPem.size()));
    auto keyBio = MakeMemBio(keyPem.data(), static_cast<int>(keyPem.size()));
    auto certX509 = MakeX509(::PEM_read_bio_X509(certBio.get(), nullptr, nullptr, nullptr));
    auto pkey = MakePKey(::PEM_read_bio_PrivateKey(keyBio.get(), nullptr, nullptr, nullptr));

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
  } else if (!cfg.cipherList().empty()) {
    if (::SSL_CTX_set_cipher_list(ctx, cfg.cipherListCstr()) != 1) {
      throw std::runtime_error("Failed to set cipher list");
    }
  }
}

void ConfigureProtocolBounds(SSL_CTX* ctx, const TLSConfig& cfg) {
  if (cfg.minVersion != TLSConfig::Version{}) {
    int mv = ParseTlsVersion(cfg.minVersion);
    if (mv == 0 || ::SSL_CTX_set_min_proto_version(ctx, mv) != 1) {
      throw std::runtime_error("Failed to set minimum TLS version");
    }
  }
  if (cfg.maxVersion != TLSConfig::Version{}) {
    int Mv = ParseTlsVersion(cfg.maxVersion);
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
    auto cbio = MakeMemBio(pem.data(), static_cast<int>(pem.size()));
    auto cx = MakeX509(::PEM_read_bio_X509(cbio.get(), nullptr, nullptr, nullptr));
    if (::SSL_CTX_get_cert_store(ctx) == nullptr) {
      throw std::runtime_error("No cert store available in SSL_CTX");
    }
    if (::X509_STORE_add_cert(::SSL_CTX_get_cert_store(ctx), cx.get()) != 1) {
      throw std::runtime_error("Failed to add trusted client certificate to store");
    }
  }
}

const int kTicketStoreIndex = []() { return ::SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr); }();

int SessionTicketCallback(SSL* ssl, unsigned char* keyName, unsigned char* iv, EVP_CIPHER_CTX* cctx, EVP_MAC_CTX* mctx,
                          int enc) {
  SSL_CTX* sslCtx = ::SSL_get_SSL_CTX(ssl);
  TlsTicketKeyStore* storePtr = static_cast<TlsTicketKeyStore*>(::SSL_CTX_get_ex_data(sslCtx, kTicketStoreIndex));
  if (storePtr == nullptr) {
    return 0;
  }
  return storePtr->processTicket(keyName, iv, EVP_MAX_IV_LENGTH, cctx, mctx, enc);
}

void ConfigureSessionTickets(SSL_CTX* ctx, const TLSConfig& cfg,
                             const std::shared_ptr<TlsTicketKeyStore>& ticketStore) {
  if (!cfg.sessionTickets.enabled) {
    if ((::SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET) & SSL_OP_NO_TICKET) == 0) {
      throw std::runtime_error("Failed to set SSL_OP_NO_TICKET on SSL_CTX");
    }
    return;
  }
  if ((::SSL_CTX_clear_options(ctx, SSL_OP_NO_TICKET) & SSL_OP_NO_TICKET) != 0) {
    throw std::runtime_error("Failed to clear SSL_OP_NO_TICKET on SSL_CTX");
  }
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
      throw std::invalid_argument("Invalid cipher policy");
  }
}

void ApplyCipherPolicy(SSL_CTX* ctx, const TLSConfig& cfg) {
  if (::SSL_CTX_set_ciphersuites(ctx, CipherPolicyTls13(cfg.cipherPolicy)) != 1) {
    throw std::runtime_error("Failed to set TLS 1.3 cipher suites");
  }
  if (::SSL_CTX_set_cipher_list(ctx, CipherPolicyTls12(cfg.cipherPolicy)) != 1) {
    throw std::runtime_error("Failed to set TLS cipher list");
  }
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
    throw std::bad_alloc();
  }
  auto* ctx = reinterpret_cast<SSL_CTX*>(_ctx.get());
  ConfigureContextOptions(ctx, cfg);
  ConfigureProtocolBounds(ctx, cfg);
  LoadCertificateAndKey(ctx, cfg.certPem(), cfg.keyPem(), cfg.certFileCstr(), cfg.keyFileCstr());
  ConfigureClientVerification(ctx, cfg);

  auto alpnProtocols = cfg.alpnProtocols();
  std::size_t wireLen = std::accumulate(alpnProtocols.begin(), alpnProtocols.end(), std::size_t{0},
                                        [](std::size_t sum, const auto& proto) { return sum + 1UL + proto.size(); });
  if (wireLen != 0) {
    _alpnData = std::make_unique<AlpnData>(RawBytes{wireLen}, cfg.alpnMustMatch, metrics);
    for (const auto& proto : alpnProtocols) {
      _alpnData->wire.unchecked_push_back(static_cast<std::byte>(proto.size()));
      _alpnData->wire.unchecked_append(reinterpret_cast<const std::byte*>(proto.data()), proto.size());
    }
    ::SSL_CTX_set_alpn_select_cb(ctx, &TlsContext::SelectAlpn, _alpnData.get());
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
        LoadCertificateAndKey(routeRaw, std::string_view{}, std::string_view{}, entry.certFileCstrView(),
                              entry.keyFileCstrView());
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
    ::SSL_CTX_set_tlsext_servername_arg(ctx, _sniRoutes.get());
    ::SSL_CTX_set_tlsext_servername_callback(ctx, &TlsContext::SelectSniRoute);
  }

  const auto opts = ::SSL_CTX_get_options(ctx);
  const bool ktlsAllowed = (opts & SSL_OP_ENABLE_KTLS) != 0;
  const bool compressionAllowed = (opts & SSL_OP_NO_COMPRESSION) == 0;
  log::debug("SSL_CTX options:");
  log::debug(" - kTLS:        {}", ktlsAllowed ? "enabled" : "disabled");
  log::debug(" - compression: {}", compressionAllowed ? "enabled" : "disabled");
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
    if (auto* obs = GetTlsHandshakeObserver(reinterpret_cast<ssl_st*>(ssl))) {
      obs->alpnStrictMismatch = true;
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
  for (auto& route : routeSpan) {
    if (MatchesSniPattern(route.pattern, route.wildcard, serverName)) {
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