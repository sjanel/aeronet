#pragma once

#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/ssl.h>

// Indirection points for a small number of OpenSSL calls that are otherwise
// extremely hard to exercise in tests (e.g. allocation failures).
//
// The library provides default implementations; unit tests may override them at
// link-time to inject failures deterministically.
extern "C" SSL* AeronetSslNew(SSL_CTX* ctx);
extern "C" int AeronetSslSetFd(SSL* ssl, int fd);

#endif
