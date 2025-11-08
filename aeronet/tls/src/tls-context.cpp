#include "tls-context.hpp"

#include <openssl/evp.h>       // EVP_PKEY_free
#include <openssl/pem.h>       // PEM_read_bio_X509, PEM_read_bio_PrivateKey
#include <openssl/prov_ssl.h>  // TLS1_2_VERSION
#include <openssl/ssl.h>       // SSL_*, SSL_read/write, handshake, shutdown, SSL_CTX, SSL_get_error
#include <openssl/tls1.h>      // TLS1_2_VERSION (for OpenSSL < 3.0, safe to include anyway)
#include <openssl/types.h>     // SSL, SSL_CTX forward declarations
#include <openssl/x509.h>      // X509_free, X509_get_subject_name, X509_NAME_oneline
#include <openssl/x509_vfy.h>  // X509_STORE_add_cert

#include <cstddef>
#include <cstring>
#include <memory>
#include <numeric>
#include <stdexcept>

#include "aeronet/tls-config.hpp"
#include "raw-bytes.hpp"
#include "tls-raii.hpp"

namespace aeronet {
namespace {
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
}  // namespace

void TlsContext::CtxDel::operator()(ssl_ctx_st* ctxPtr) const noexcept {
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  ::SSL_CTX_free(reinterpret_cast<SSL_CTX*>(ctxPtr));
}

TlsContext::~TlsContext() = default;

TlsContext::TlsContext(const TLSConfig& cfg, TlsMetricsExternal* metrics) : _ctx(::SSL_CTX_new(TLS_server_method())) {
  if (!_ctx) {
    throw std::runtime_error("SSL_CTX_new failed");
  }
  auto* raw = reinterpret_cast<SSL_CTX*>(_ctx.get());
#if defined(AERONET_ENABLE_KTLS) && defined(SSL_OP_ENABLE_KTLS)
  if (cfg.ktlsMode != TLSConfig::KtlsMode::Disabled) {
    ::SSL_CTX_set_options(raw, SSL_OP_ENABLE_KTLS);
  }
#endif
  if (!cfg.cipherList().empty()) {
    if (::SSL_CTX_set_cipher_list(raw, cfg.cipherListCstrView().c_str()) != 1) {
      throw std::runtime_error("Failed to set cipher list");
    }
  }
  // Protocol version bounds if provided
  if (cfg.minVersion != TLSConfig::Version{}) {
    int mv = parseTlsVersion(cfg.minVersion);
    if (mv == 0 || ::SSL_CTX_set_min_proto_version(raw, mv) != 1) {
      throw std::runtime_error("Failed to set minimum TLS version");
    }
  }
  if (cfg.maxVersion != TLSConfig::Version{}) {
    int Mv = parseTlsVersion(cfg.maxVersion);
    if (Mv == 0 || ::SSL_CTX_set_max_proto_version(raw, Mv) != 1) {
      throw std::runtime_error("Failed to set maximum TLS version");
    }
  }
  std::string_view certPem = cfg.certPem();
  std::string_view keyPem = cfg.keyPem();
  if (!certPem.empty() && !keyPem.empty()) {
    // In-memory load path
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
    // SSL_CTX increases ref counts internally; unique_ptr releases will free local if ref counts allow.
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
  if (cfg.requestClientCert) {
    int verifyMode = SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE;
    if (cfg.requireClientCert) {
      verifyMode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
    }
    ::SSL_CTX_set_verify(raw, verifyMode, nullptr);
    // Load any in-memory trusted client certs (appended to default store). For test / pinning usage.
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
      if (::SSL_CTX_get_cert_store(raw) == nullptr) {
        throw std::runtime_error("No cert store available in SSL_CTX");
      }
      if (::X509_STORE_add_cert(::SSL_CTX_get_cert_store(raw), cx.get()) != 1) {
        throw std::runtime_error("Failed to add trusted client certificate to store");
      }
    }
  }

  // ALPN setup
  auto alpnProtocols = cfg.alpnProtocols();

  std::size_t wireLen = std::accumulate(alpnProtocols.begin(), alpnProtocols.end(), std::size_t{0},
                                        [](std::size_t sum, const auto& proto) { return sum + 1UL + proto.size(); });
  if (wireLen != 0) {
    _alpnData = std::make_unique<AlpnData>(RawBytes{wireLen}, cfg.alpnMustMatch, metrics);
    for (const auto& proto : alpnProtocols) {
      _alpnData->wire.unchecked_push_back(static_cast<std::byte>(proto.size()));
      _alpnData->wire.unchecked_append(reinterpret_cast<const std::byte*>(proto.data()), proto.size());
    }
    ::SSL_CTX_set_alpn_select_cb(
        raw,
        []([[maybe_unused]] SSL* ssl, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
           unsigned int inlen, void* arg) -> int {
          auto* data = reinterpret_cast<AlpnData*>(arg);
          const unsigned char* ptr = reinterpret_cast<const unsigned char*>(data->wire.data());
          unsigned int prefLen = static_cast<unsigned int>(data->wire.size());
          unsigned int prefIndex = 0;
          while (prefIndex < prefLen) {
            unsigned int len = ptr[prefIndex];
            const unsigned char* val = ptr + prefIndex + 1;
            unsigned int clientIndex = 0;
            while (clientIndex < inlen) {
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
            if (data->metrics) {
              ++data->metrics->alpnStrictMismatches;
            }
            return SSL_TLSEXT_ERR_ALERT_FATAL;
          }
          return SSL_TLSEXT_ERR_NOACK;
        },
        _alpnData.get());
  }
}

}  // namespace aeronet