#include "aeronet/test-tls-helper.hpp"

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/types.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace aeronet::test {

namespace {

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&::EVP_PKEY_free)>;
using PkeyCtxPtr = std::unique_ptr<EVP_PKEY_CTX, decltype(&::EVP_PKEY_CTX_free)>;

PkeyPtr GenerateKey(KeyAlgorithm alg) {
  if (alg == KeyAlgorithm::Rsa2048) {
    EVP_PKEY* pkey = nullptr;
    PkeyCtxPtr kctx(::EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), ::EVP_PKEY_CTX_free);
    if (kctx == nullptr) {
      return {nullptr, ::EVP_PKEY_free};
    }
    if (::EVP_PKEY_keygen_init(kctx.get()) != 1 || ::EVP_PKEY_CTX_set_rsa_keygen_bits(kctx.get(), 2048) != 1 ||
        ::EVP_PKEY_keygen(kctx.get(), &pkey) != 1) {
      return {nullptr, ::EVP_PKEY_free};
    }
    return {pkey, ::EVP_PKEY_free};
  }

  // ECDSA P-256
  EVP_PKEY* pkey = nullptr;
  PkeyCtxPtr kctx(::EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr), ::EVP_PKEY_CTX_free);
  if (kctx == nullptr) {
    return {nullptr, ::EVP_PKEY_free};
  }
  if (::EVP_PKEY_keygen_init(kctx.get()) != 1) {
    return {nullptr, ::EVP_PKEY_free};
  }
  if (::EVP_PKEY_CTX_set_ec_paramgen_curve_nid(kctx.get(), NID_X9_62_prime256v1) != 1) {
    return {nullptr, ::EVP_PKEY_free};
  }
  if (::EVP_PKEY_keygen(kctx.get(), &pkey) != 1) {
    return {nullptr, ::EVP_PKEY_free};
  }
  return {pkey, ::EVP_PKEY_free};
}

using X509Ptr = std::unique_ptr<X509, decltype(&::X509_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&::BIO_free)>;

std::string BioContent(BIO* bio) {
  char* data = nullptr;
  const long len = BIO_get_mem_data(bio, &data);
  return {data, static_cast<std::size_t>(len)};
}

std::string CertPem(X509* x509) {
  BioPtr bio(::BIO_new(::BIO_s_mem()), ::BIO_free);
  if (!bio || ::PEM_write_bio_X509(bio.get(), x509) != 1) {
    throw std::runtime_error("PEM_write_bio_X509 failed");
  }
  return BioContent(bio.get());
}

std::string KeyPem(EVP_PKEY* pkey) {
  BioPtr bio(::BIO_new(::BIO_s_mem()), ::BIO_free);
  if (!bio || ::PEM_write_bio_PrivateKey(bio.get(), pkey, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
    throw std::runtime_error("PEM_write_bio_PrivateKey failed");
  }
  return BioContent(bio.get());
}

void AddExtension(X509* cert, X509* issuer, int nid, const char* value) {
  X509V3_CTX v3ctx;
  X509V3_set_ctx_nodb(&v3ctx);
  X509V3_set_ctx(&v3ctx, issuer, cert, nullptr, nullptr, 0);
  X509_EXTENSION* ext = ::X509V3_EXT_conf_nid(nullptr, &v3ctx, nid, value);
  if (ext == nullptr || ::X509_add_ext(cert, ext, -1) != 1) {
    ::X509_EXTENSION_free(ext);
    throw std::runtime_error("Failed to add a certificate extension");
  }
  ::X509_EXTENSION_free(ext);
}

// Certificate of `pkey` named `commonName`, signed by issuerKey (self-signed when issuer is null).
X509Ptr MakeCertificate(const char* commonName, EVP_PKEY* pkey, X509* issuer, EVP_PKEY* issuerKey, bool isCa,
                        long serial) {
  X509Ptr cert(::X509_new(), ::X509_free);
  if (!cert) {
    throw std::runtime_error("X509_new failed");
  }
  ::X509_set_version(cert.get(), 2);  // v3
  ::ASN1_INTEGER_set(::X509_get_serialNumber(cert.get()), serial);
  ::X509_gmtime_adj(::X509_getm_notBefore(cert.get()), -60);
  ::X509_gmtime_adj(::X509_getm_notAfter(cert.get()), 3600);
  ::X509_set_pubkey(cert.get(), pkey);
  X509_NAME* name = ::X509_get_subject_name(cert.get());
  ::X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("AeronetTest"), -1, -1,
                               0);
  ::X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(commonName), -1, -1, 0);
  ::X509_set_issuer_name(cert.get(), issuer == nullptr ? name : ::X509_get_subject_name(issuer));
  X509* issuerCert = issuer == nullptr ? cert.get() : issuer;
  if (isCa) {
    AddExtension(cert.get(), issuerCert, NID_basic_constraints, "critical,CA:TRUE");
    AddExtension(cert.get(), issuerCert, NID_key_usage, "critical,keyCertSign,cRLSign");
  } else {
    const std::string san = std::string("DNS:") + commonName;
    AddExtension(cert.get(), issuerCert, NID_basic_constraints, "CA:FALSE");
    AddExtension(cert.get(), issuerCert, NID_subject_alt_name, san.c_str());
    AddExtension(cert.get(), issuerCert, NID_ext_key_usage, "serverAuth,clientAuth");
  }
  AddExtension(cert.get(), issuerCert, NID_subject_key_identifier, "hash");
  if (::X509_sign(cert.get(), issuerKey, ::EVP_sha256()) <= 0) {
    throw std::runtime_error("X509_sign failed");
  }
  return cert;
}

}  // namespace

CertificateChain MakeCertificateChain(const char* leafCommonName) {
  auto rootKey = GenerateKey(KeyAlgorithm::EcdsaP256);
  auto intermediateKey = GenerateKey(KeyAlgorithm::EcdsaP256);
  auto leafKey = GenerateKey(KeyAlgorithm::EcdsaP256);
  if (!rootKey || !intermediateKey || !leafKey) {
    throw std::runtime_error("Key generation failed");
  }
  auto root = MakeCertificate("Aeronet Test Root CA", rootKey.get(), nullptr, rootKey.get(), true, 1);
  auto intermediate =
      MakeCertificate("Aeronet Test Intermediate CA", intermediateKey.get(), root.get(), rootKey.get(), true, 2);
  auto leaf = MakeCertificate(leafCommonName, leafKey.get(), intermediate.get(), intermediateKey.get(), false, 3);

  CertificateChain chain;
  chain.rootCertPem = CertPem(root.get());
  chain.intermediateCertPem = CertPem(intermediate.get());
  chain.leafCertPem = CertPem(leaf.get());
  chain.leafKeyPem = KeyPem(leafKey.get());
  return chain;
}

std::string EncryptPrivateKeyPem(const std::string& keyPem, const char* passphrase) {
  BioPtr in(::BIO_new_mem_buf(keyPem.data(), static_cast<int>(keyPem.size())), ::BIO_free);
  PkeyPtr pkey(in ? ::PEM_read_bio_PrivateKey(in.get(), nullptr, nullptr, nullptr) : nullptr, ::EVP_PKEY_free);
  BioPtr out(::BIO_new(::BIO_s_mem()), ::BIO_free);
  if (!pkey || !out ||
      ::PEM_write_bio_PKCS8PrivateKey(out.get(), pkey.get(), ::EVP_aes_256_cbc(), nullptr, 0, nullptr,
                                      const_cast<char*>(passphrase)) != 1) {
    throw std::runtime_error("Failed to encrypt the private key");
  }
  return BioContent(out.get());
}

std::pair<std::string, std::string> MakeEphemeralCertKey(const char* commonName, int validSeconds, KeyAlgorithm alg) {
  auto pkey = GenerateKey(alg);
  if (!pkey) {
    return {"", ""};
  }

  std::unique_ptr<X509, decltype(&X509_free)> x509Ptr(X509_new(), &X509_free);

  X509* x509 = x509Ptr.get();
  if (x509 == nullptr) {
    return {"", ""};
  }
  ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
  X509_gmtime_adj(X509_get_notBefore(x509), 0);
  X509_gmtime_adj(X509_get_notAfter(x509), validSeconds);
  X509_set_pubkey(x509, pkey.get());
  X509_NAME* name = X509_get_subject_name(x509);
  X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("XX"), -1, -1, 0);
  X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("AeronetTest"), -1, -1, 0);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(commonName), -1, -1, 0);
  X509_set_issuer_name(x509, name);
  if (X509_sign(x509, pkey.get(), EVP_sha256()) <= 0) {
    return {"", ""};
  }

  std::string certPem;
  std::string keyPem;
  {
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio != nullptr) {
      if (PEM_write_bio_X509(bio, x509) == 1) {
        char* data = nullptr;
        long len = BIO_get_mem_data(bio, &data);
        certPem.assign(data, static_cast<std::size_t>(len));
      }
      BIO_free(bio);
    }
  }
  {
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio != nullptr) {
      if (PEM_write_bio_PrivateKey(bio, pkey.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1) {
        char* data = nullptr;
        long len = BIO_get_mem_data(bio, &data);
        keyPem.assign(data, static_cast<std::size_t>(len));
      }
      BIO_free(bio);
    }
  }
  return {certPem, keyPem};
}

}  // namespace aeronet::test
