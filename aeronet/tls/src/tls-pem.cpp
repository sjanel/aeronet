#include "aeronet/tls-pem.hpp"

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/types.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <cassert>
#include <format>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/tls-raii.hpp"

namespace aeronet {

namespace {

// Passphrase callback rejecting encrypted private keys. Without a callback, OpenSSL prompts for the passphrase on the
// terminal, which would block the event loop rebuilding a context on a configuration reload.
int RejectPassphrase([[maybe_unused]] char* buf, [[maybe_unused]] int size, [[maybe_unused]] int rwflag,
                     [[maybe_unused]] void* userData) {
  return 0;
}

// Calls onCertificate(X509Ptr) for each PEM certificate of `pem`, in order. Throws if there is none, or if one cannot
// be parsed (text that is not a PEM block after the last certificate is ignored, like OpenSSL does for files).
template <class OnCertificate>
void ForEachPemCertificate(std::string_view pem, std::string_view what, OnCertificate&& onCertificate) {
  auto bio = MakeMemBio(pem.data(), static_cast<int>(pem.size()));
  bool hasCertificate = false;
  // The _AUX variant also accepts the "TRUSTED CERTIFICATE" blocks of the OpenSSL trust format.
  for (X509Ptr cert(::PEM_read_bio_X509_AUX(bio.get(), nullptr, RejectPassphrase, nullptr), ::X509_free); cert;
       cert.reset(::PEM_read_bio_X509_AUX(bio.get(), nullptr, RejectPassphrase, nullptr))) {
    onCertificate(std::move(cert));
    hasCertificate = true;
  }
  // The reads stop at the end of the data, reported as a missing PEM block, or at the first malformed certificate.
  const unsigned long lastError = ::ERR_peek_last_error();
  if (!hasCertificate || ERR_GET_LIB(lastError) != ERR_LIB_PEM || ERR_GET_REASON(lastError) != PEM_R_NO_START_LINE) {
    ThrowOpenSslError(std::format("Failed to parse {}", what));
  }
  ::ERR_clear_error();
}

}  // namespace

void ThrowOpenSslError(std::string_view context) {
  char reason[256];
  ::ERR_error_string_n(::ERR_peek_error(), reason, sizeof(reason));
  ::ERR_clear_error();
  throw std::runtime_error(std::format("{}: {}", context, std::string_view(reason)));
}

void UseCertificateChainPem(SSL_CTX* ctx, std::string_view certPem, std::string_view keyPem) {
  bool isLeaf = true;
  ForEachPemCertificate(certPem, "the in-memory TLS certificate", [ctx, &isLeaf](X509Ptr cert) {
    if (isLeaf) {
      if (::SSL_CTX_use_certificate(ctx, cert.get()) != 1) {
        ThrowOpenSslError("Failed to use the in-memory TLS certificate");
      }
      isLeaf = false;
    } else if (::SSL_CTX_add0_chain_cert(ctx, cert.get()) == 1) {
      (void)cert.release();  // owned by the SSL_CTX now
    } else {
      ThrowOpenSslError("Failed to add an intermediate certificate of the in-memory TLS certificate chain");
    }
  });

  auto keyBio = MakeMemBio(keyPem.data(), static_cast<int>(keyPem.size()));
  PKeyPtr pkey(::PEM_read_bio_PrivateKey(keyBio.get(), nullptr, RejectPassphrase, nullptr), ::EVP_PKEY_free);
  if (!pkey) {
    ThrowOpenSslError("Failed to parse the in-memory TLS private key");
  }
  // SSL_CTX_use_PrivateKey only fails for a key not matching a certificate of the same algorithm: a key of another
  // algorithm (an ECDSA key for an RSA certificate) is only rejected by SSL_CTX_check_private_key.
  if (::SSL_CTX_use_PrivateKey(ctx, pkey.get()) != 1 || ::SSL_CTX_check_private_key(ctx) != 1) {
    ThrowOpenSslError("Failed to use the in-memory TLS private key");
  }
}

void UseCertificateChainFiles(SSL_CTX* ctx, const char* certFile, const char* keyFile) {
  // Used to decrypt the PEM blocks of both files.
  ::SSL_CTX_set_default_passwd_cb(ctx, RejectPassphrase);
  if (::SSL_CTX_use_certificate_chain_file(ctx, certFile) != 1) {
    ThrowOpenSslError(std::format("Failed to load TLS certificate file '{}'", certFile));
  }
  // See UseCertificateChainPem() for the key check.
  if (::SSL_CTX_use_PrivateKey_file(ctx, keyFile, SSL_FILETYPE_PEM) != 1 || ::SSL_CTX_check_private_key(ctx) != 1) {
    ThrowOpenSslError(std::format("Failed to load TLS private key file '{}'", keyFile));
  }
}

void AddTrustedCertificatesPem(SSL_CTX* ctx, std::string_view pem) {
  X509_STORE* store = ::SSL_CTX_get_cert_store(ctx);
  // A freshly created SSL_CTX always has a cert store.
  assert(store != nullptr && "SSL_CTX missing cert store");
  ForEachPemCertificate(pem, "a trusted TLS certificate", [ctx, store](const X509Ptr& cert) {
    if (::X509_STORE_add_cert(store, cert.get()) != 1) {
      ThrowOpenSslError("Failed to add a trusted TLS certificate to the verification store");
    }
    if (::SSL_CTX_add_client_CA(ctx, cert.get()) != 1) {
      ThrowOpenSslError("Failed to add a trusted TLS certificate to the CA list of the certificate request");
    }
  });
}

}  // namespace aeronet
