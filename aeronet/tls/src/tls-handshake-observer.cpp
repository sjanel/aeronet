#include "aeronet/tls-handshake-observer.hpp"

#include <openssl/ssl.h>
#include <openssl/types.h>

namespace aeronet {

namespace {

const int kIndex = []() { return ::SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr); }();

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
