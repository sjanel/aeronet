#include "aeronet/tls-handshake-observer.hpp"

#include <openssl/ssl.h>
#include <openssl/types.h>

namespace aeronet {

namespace {

// OpenSSL is a C library. SSL_get_ex_new_index() cannot throw C++ exceptions;
// suppress the false positive from clang-tidy.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const int kIndex = ::SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);

}  // namespace

int SetTlsHandshakeObserver(ssl_st* ssl, TlsHandshakeObserver* observer) noexcept {
  // SSL_set_ex_data returns 1 on success, 0 on failure. Propagate that to callers so they
  // can handle allocation/registration failures (rare but possible).
  return ::SSL_set_ex_data(reinterpret_cast<SSL*>(ssl), kIndex, observer);
}

TlsHandshakeObserver* GetTlsHandshakeObserver(ssl_st* ssl) noexcept {
  return static_cast<TlsHandshakeObserver*>(::SSL_get_ex_data(reinterpret_cast<SSL*>(ssl), kIndex));
}

}  // namespace aeronet
