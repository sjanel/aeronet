#pragma once

#include <string_view>

// Forward declare OpenSSL context struct (avoid pulling heavy headers into public interface).
struct ssl_ctx_st;  // SSL_CTX

namespace aeronet {

// Throws a std::runtime_error made of `context` and the reason of the first error of the OpenSSL error queue (the root
// cause), after emptying this queue.
[[noreturn]] void ThrowOpenSslError(std::string_view context);

// Installs a certificate chain and its private key in an SSL_CTX, from in-memory PEM data.
// `certPem` holds the leaf certificate first, then the intermediate certificates sent with it (a "full chain").
// An encrypted private key is rejected, instead of OpenSSL prompting for its passphrase on the terminal.
// Throws std::runtime_error, with the OpenSSL reason, if a PEM block cannot be parsed or the key does not match the
// certificate. Leaves the OpenSSL error queue of the calling thread empty.
void UseCertificateChainPem(ssl_ctx_st* ctx, std::string_view certPem, std::string_view keyPem);

// Same as UseCertificateChainPem(), from PEM files. The error names the faulty file.
void UseCertificateChainFiles(ssl_ctx_st* ctx, const char* certFile, const char* keyFile);

// Adds every certificate of `pem` (one PEM certificate or a bundle of them) to the verification store of `ctx`, and
// their subject names to the list of acceptable CAs that a server sends in its certificate request.
// Throws std::runtime_error, with the OpenSSL reason, if `pem` holds no certificate or a malformed one.
void AddTrustedCertificatesPem(ssl_ctx_st* ctx, std::string_view pem);

}  // namespace aeronet
