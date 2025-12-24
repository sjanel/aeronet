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

#include <cstddef>
#include <memory>
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

}  // namespace

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
