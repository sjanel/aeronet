#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

struct ssl_st;   // SSL
struct x509_st;  // X509

namespace aeronet::test {

// Shared test utility for generating ephemeral self-signed certificates entirely in memory.
// Returns {certPem, keyPem}. Intended ONLY for tests – no persistence.
// Default is RSA 2048-bit, 1h validity. The certificate carries a "DNS:<commonName>" subject alternative name.
enum class KeyAlgorithm : uint8_t { Rsa2048, EcdsaP256 };

std::pair<std::string, std::string> MakeEphemeralCertKey(const char* commonName = "localhost", int validSeconds = 3600,
                                                         KeyAlgorithm alg = KeyAlgorithm::Rsa2048);

// A three-level PKI (ECDSA P-256, 1h validity): a self-signed root CA, an intermediate CA signed by the root, and a
// leaf certificate signed by the intermediate one (with a "DNS:<leafCommonName>" subject alternative name).
struct CertificateChain {
  // The leaf certificate followed by the intermediate one, as served by a TLS endpoint.
  [[nodiscard]] std::string fullChainPem() const { return leafCertPem + intermediateCertPem; }

  std::string rootCertPem;
  std::string intermediateCertPem;
  std::string leafCertPem;
  std::string leafKeyPem;
};

CertificateChain MakeCertificateChain(const char* pLeafCommonName = "localhost");

// The PEM private key `keyPem`, encrypted (AES-256-CBC) with `passphrase`.
std::string EncryptPrivateKeyPem(const std::string& keyPem, const char* pPassphrase);

// Names `cert` "CN=<commonName>" and makes it self-issued (its issuer name is its subject name).
void SetSelfIssuedName(x509_st* cert, const char* pCommonName);

// The first common name (CN) of the subject of `cert`, or an empty string if it has none.
std::string SubjectCommonName(const x509_st* pCert);

// Reads at most `maxBytes` from `pSsl` (SSL_read_ex), appending them to `out`. Returns the number of appended bytes, 0
// when nothing was read, SSL_get_error(pSsl, 0) then telling why. Failures are logged, except when the read would block
// and when the peer closed the connection (close_notify).
std::size_t SslReadAppend(ssl_st* pSsl, std::string& out, std::size_t maxBytes = 16384);

}  // namespace aeronet::test
