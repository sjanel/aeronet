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

// SSL_set_accept_state(), returning 1 on success, 0 on failure: since OpenSSL 3.2, it re-creates the record layers of
// the connection and ignores an allocation failure there, which leaves the connection in error without record layer
// (a later SSL_ctrl(), like SSL_set_mode(), would dereference it).
extern "C" int AeronetSslSetAcceptState(SSL* ssl);

#endif
