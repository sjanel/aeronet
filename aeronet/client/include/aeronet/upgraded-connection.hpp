#pragma once

#include <expected>

#include "aeronet/connection.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/transport.hpp"

namespace aeronet {

// A connection switched to another protocol by a 101 (Switching Protocols) response, see HttpClient::upgrade().
struct UpgradedConnection {
  // Whether the server switched protocols: the connection is then open, and owned by this object.
  [[nodiscard]] bool valid() const noexcept { return static_cast<bool>(cnx); }

  // The transport of the connection, plain or TLS (handshake done). TLS may already hold decrypted bytes of the new
  // protocol (Transport::hasPendingReadData()): they are read before waiting for the socket to be readable.
  Transport transport;
  // The bytes of the new protocol received with the head of the 101 response, to process before any read (often none).
  RawChars received;
  // The socket of the connection.
  Connection cnx;
};

// Outcome of a successful HttpClient::upgrade() exchange.
struct HttpUpgradeResponse {
  // The response of the server: the head of the 101 response when 'connection' is valid, else the whole final response
  // of a server that did not switch protocols (a 401, a 429 with Retry-After...).
  HttpResponse response;
  UpgradedConnection connection;
};

// Result of HttpClient::upgrade(): the response, or the HttpClientErrc of a failed exchange.
using HttpUpgradeResult = std::expected<HttpUpgradeResponse, HttpClientErrc>;

}  // namespace aeronet
