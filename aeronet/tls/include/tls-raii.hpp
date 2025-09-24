#pragma once

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <memory>

namespace aeronet {

// Generic RAII aliases (function pointer deleters keep type size = one pointer)
using SslCtxPtr = std::unique_ptr<SSL_CTX, decltype(&::SSL_CTX_free)>;
using SslPtr = std::unique_ptr<SSL, decltype(&::SSL_free)>;
using BioPtr = std::unique_ptr<BIO, decltype(&::BIO_free)>;
using X509Ptr = std::unique_ptr<X509, decltype(&::X509_free)>;
using PKeyPtr = std::unique_ptr<EVP_PKEY, decltype(&::EVP_PKEY_free)>;

// Helpers
inline BioPtr makeBio(BIO* bio) noexcept { return BioPtr(bio, ::BIO_free); }

inline BioPtr makeMemBio(const void* data, int len) noexcept { return makeBio(BIO_new_mem_buf(data, len)); }

// Allocate an empty memory BIO (equivalent to BIO_new(BIO_s_mem())) with RAII.
inline BioPtr makeMemoryBio() noexcept { return makeBio(BIO_new(BIO_s_mem())); }

}  // namespace aeronet
