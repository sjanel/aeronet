#pragma once

// Forward declare OpenSSL SSL type to avoid including heavy OpenSSL headers here.
struct ssl_st;

namespace aeronet {

// Small per-connection observer updated by OpenSSL callbacks via SSL ex_data.
// Stored in ConnectionState and used to bucket handshake failures.
struct TlsHandshakeObserver {
  bool alpnStrictMismatch{false};
};

// Associate an observer with an SSL object. Safe to call multiple times.
int SetTlsHandshakeObserver(ssl_st* ssl, TlsHandshakeObserver* observer) noexcept;

// Retrieve the observer pointer (may be nullptr).
TlsHandshakeObserver* GetTlsHandshakeObserver(ssl_st* ssl) noexcept;

}  // namespace aeronet
