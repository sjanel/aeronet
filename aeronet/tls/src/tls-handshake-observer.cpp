#include "aeronet/tls-handshake-observer.hpp"

#include <openssl/ssl.h>
#include <openssl/types.h>

namespace aeronet {

namespace {

// SSL ex data index, allocated on first use: a TLS server may be started during the static initialization of another
// translation unit (e.g. a global server), possibly before namespace-scope variables of this one are initialized (the
// index would still be 0, the application data slot). Function-local statics are thread-safe.
int ObserverIndex() noexcept {
  static const int kIndex = ::SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
  return kIndex;
}

}  // namespace

int SetTlsHandshakeObserver(ssl_st* ssl, TlsHandshakeObserver* observer) noexcept {
  // SSL_set_ex_data returns 1 on success, 0 on failure. Propagate that to callers so they
  // can handle allocation/registration failures (rare but possible).
  return ::SSL_set_ex_data(reinterpret_cast<SSL*>(ssl), ObserverIndex(), observer);
}

TlsHandshakeObserver* GetTlsHandshakeObserver(ssl_st* ssl) noexcept {
  return static_cast<TlsHandshakeObserver*>(::SSL_get_ex_data(reinterpret_cast<SSL*>(ssl), ObserverIndex()));
}

}  // namespace aeronet
