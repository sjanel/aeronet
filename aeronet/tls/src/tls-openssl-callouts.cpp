#include "aeronet/tls-openssl-callouts.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/types.h>

#if defined(__GNUC__) || defined(__clang__)
#define AERONET_TLS_WEAK __attribute__((weak))
#else
#define AERONET_TLS_WEAK
#endif

extern "C" AERONET_TLS_WEAK SSL* AeronetSslNew(SSL_CTX* ctx) { return ::SSL_new(ctx); }

extern "C" AERONET_TLS_WEAK int AeronetSslSetFd(SSL* ssl, int fd) { return ::SSL_set_fd(ssl, fd); }

extern "C" AERONET_TLS_WEAK int AeronetSslSetAcceptState(SSL* ssl) {
  ::SSL_set_accept_state(ssl);
  // A failure puts the connection in error, out of the 'before' state it is otherwise left in.
  return ::SSL_in_before(ssl);
}

#endif
