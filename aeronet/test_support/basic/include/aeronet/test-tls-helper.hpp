#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace aeronet::test {

// Shared test utility for generating ephemeral self-signed certificates entirely in memory.
// Returns {certPem, keyPem}. Intended ONLY for tests – no persistence.
// Default is RSA 2048-bit, 1h validity.
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

CertificateChain MakeCertificateChain(const char* leafCommonName = "localhost");

// The PEM private key `keyPem`, encrypted (AES-256-CBC) with `passphrase`.
std::string EncryptPrivateKeyPem(const std::string& keyPem, const char* passphrase);

}  // namespace aeronet::test
