#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/connection.hpp"
#include "aeronet/event.hpp"
#include "aeronet/http-client-error.hpp"
#include "aeronet/http-request.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/native-handle.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/tcp-connector.hpp"
#include "aeronet/timedef.hpp"
#include "aeronet/transport.hpp"
#include "aeronet/upgraded-connection.hpp"
#include "http1-response-parser.hpp"

struct addrinfo;

namespace aeronet {

class HttpClient;

}  // namespace aeronet

namespace aeronet::internal {

// The exchange switching a new connection to another protocol (see HttpClient::upgrade()), as a state machine driven by
// the readiness of its socket: HttpClient::upgrade() runs it to completion, and WebSocketClient interleaves the steps
// of its handshakes with the service of its open connections. An attempt goes through the resolution of the host to
// connect to (the forward proxy, or the server), the TCP connect (falling back to the next address when it fails), the
// CONNECT tunnel through the proxy, the TLS handshake (ALPN offering http/1.1 only), the write of the request and the
// read of its response (also read during the write: a server may refuse the request before reading all of it). Each
// phase is bounded by the timeouts of HttpClientConfig, as for a regular request: connectTimeout for the resolution,
// the connect, the tunnel and the TLS handshake each, then requestTimeout for the exchange, from the end of the TLS
// handshake, inactivityTimeout between its reads and writes. The driver gives the client to each call: the handshake
// keeps no reference to it, so that the client can move between them.
//
// The driver watches fd() for interest() in its event loop, and calls advance() when it signals, or expire() once
// deadline() passed. The socket is only closed by start(), reset() or the destructor, and
// replaced by advance() when mayReplaceSocket() tells so: the driver stops watching it, at the latest before waiting
// for the next socket (its event loop must forget the closed one, whose number the next socket may reuse).
class UpgradeHandshake {
 public:
  enum class Phase : uint8_t {
    Idle,          // no attempt in progress: start() begins one
    Resolve,       // waiting for the addresses of hostToResolve(), given to onResolved()
    Connect,       // TCP connect in progress
    ProxyWrite,    // writing the CONNECT request to the forward proxy
    ProxyRead,     // reading the answer of the proxy
    TlsHandshake,  // TLS handshake with the server
    WriteRequest,  // writing the request, reading the response if it comes before the end of the request
    ReadResponse,  // reading the response
    Done,          // response() received: the server switched protocols when it is a 101 (takeConnection())
    Failed,        // error() tells why
  };

  // Throws std::invalid_argument if 'request' has a file body or trailers: an upgrade request (a GET) only sends its
  // head and its inline body.
  static void CheckRequest(const HttpRequest& request);

  // A handshake sending 'request', made by HttpClient::makeUpgradeRequest() and accepted by CheckRequest(). The request
  // is kept for the next attempts.
  explicit UpgradeHandshake(HttpRequest request) noexcept : _request(std::move(request)) {}

  // The parser refers to a buffer of this object.
  UpgradeHandshake(const UpgradeHandshake&) = delete;
  UpgradeHandshake(UpgradeHandshake&&) = delete;
  UpgradeHandshake& operator=(const UpgradeHandshake&) = delete;
  UpgradeHandshake& operator=(UpgradeHandshake&&) = delete;

  ~UpgradeHandshake() = default;

  // Start a new attempt (the socket of a previous one is closed: it must not be watched anymore), in the Resolve phase.
  // The request is sent with the cookies of the jar of 'client' at this time, and with 'extraLines' (complete header
  // lines, each starting with CRLF) for this attempt only. Throws std::logic_error for an https request in a build
  // without OpenSSL, HttpClientException for an invalid TLS configuration.
  void start(HttpClient& client, std::string_view extraLines);

  // The host to resolve in the Resolve phase (the proxy, or the host of the request), followed by a writable byte (see
  // ResolveTCP()), and its port.
  [[nodiscard]] std::span<char> hostToResolve(const HttpClient& client) const noexcept;
  [[nodiscard]] uint16_t portToResolve(const HttpClient& client) const noexcept;

  // Continue the Resolve phase with the addresses of hostToResolve(), null when the resolution failed.
  void onResolved(HttpClient& client, AddrInfoPtr addresses);

  // Continue after fd() signaled 'events' (some of interest(), or an error).
  void advance(HttpClient& client, EventBmp events);

  // End the attempt with 'error'.
  void fail(HttpClientErrc error) noexcept;

  // End the attempt once deadline() passed: HttpClientErrc::connectFailed during the resolution and the connect, as a
  // regular request, timeout after.
  void expire() noexcept { fail(_phase <= Phase::Connect ? HttpClientErrc::connectFailed : HttpClientErrc::timeout); }

  // Run an attempt to completion on the calling thread: resolution, then waits for the socket in the event loop of
  // 'client', which no longer watches it when this returns or throws. See start() for 'extraLines'.
  void run(HttpClient& client, std::string_view extraLines);

  // Drop the attempt (its socket is closed: it must not be watched anymore) and release its memory, back to the Idle
  // phase. The request is kept.
  void reset() noexcept;

  [[nodiscard]] Phase phase() const noexcept { return _phase; }

  // Whether an attempt is in progress, from the resolution to the response.
  [[nodiscard]] bool inProgress() const noexcept { return _phase > Phase::Idle && _phase < Phase::Done; }

  // Whether advance() may close the socket and open another one (the connect to the next address).
  [[nodiscard]] bool mayReplaceSocket() const noexcept { return _phase == Phase::Connect; }

  // The socket of the attempt, kInvalidHandle before the connect.
  [[nodiscard]] NativeHandle fd() const noexcept { return _cnx.fd(); }

  // The events of fd() that the phase waits for.
  [[nodiscard]] EventBmp interest() const noexcept { return _interest; }

  // The end of the phase: fail() it with HttpClientErrc::timeout once passed.
  [[nodiscard]] SteadyClock::time_point deadline() const noexcept { return _deadline; }

  // Why the attempt failed (Failed phase), noError otherwise.
  [[nodiscard]] HttpClientErrc error() const noexcept { return _error; }

  // The response of the server (Done phase): the head of the 101, or the whole refusal.
  [[nodiscard]] HttpResponse& response() noexcept { return _response; }

  // The request of the handshake, as sent (the cookies of the jar and the extra lines are only added on the wire).
  [[nodiscard]] HttpRequest& request() noexcept { return _request; }

  // The connection switched protocols (Done phase with a 101 response): its transport, its socket, and the bytes of the
  // new protocol received with the response. The handshake no longer owns them.
  [[nodiscard]] UpgradedConnection takeConnection() noexcept;

 private:
  // Start the connect to _pAddress, or to the next addresses when it fails at once.
  void connect(HttpClient& client);

  // The TCP connection is established: start the tunnel, the TLS handshake or the request.
  void connected(HttpClient& client);

  // Enter the first phase of the exchange with the server, once a tunnel opened, if any: TLS handshake, or write of the
  // request, run by proceed().
  void startSession(HttpClient& client);

  // Start the exchange (the write of the request, then the read of its response), once the TLS handshake is done if
  // any: requestTimeout bounds it from now.
  void startExchange(const HttpClient& client) noexcept;

  // Run the phases as far as they go without waiting. 'readable': the socket signaled its readability (or an error),
  // read first while the request is written.
  void proceed(HttpClient& client, bool readable);

  // Write _out up to 'end'. Returns whether all of it is written: otherwise the attempt waits for interest(), or failed
  // with 'error'.
  bool writeOut(HttpClient& client, std::size_t end, HttpClientErrc error);

  // Read and check the answer of the proxy. Returns whether the tunnel is open: otherwise the attempt waits for
  // interest(), or failed.
  bool readProxyResponse();

  // Read and parse the response of the server, until it is complete or the read blocks.
  void readResponse(HttpClient& client);

  // The exchange made progress: push back its inactivity deadline.
  void exchangeProgressed(const HttpClient& client) noexcept;

  HttpRequest _request;
  HttpResponse _response;
  RawChars _out;         // the CONNECT request to the proxy (_proxyRequestSize first bytes), then the request
  RawChars _in;          // the answer of the proxy, then the response and the first bytes of the new protocol
  RawChars _bodyBuffer;  // reassembly of a chunked body (of a refusal)
  Http1ResponseParser _parser{_bodyBuffer};
  AddrInfoPtr _addresses;
  const addrinfo* _pAddress{nullptr};  // address of the connect in progress
  Transport _transport;
  SteadyClock::time_point _deadline;
  SteadyClock::time_point _exchangeDeadline;  // requestTimeout
  std::size_t _proxyRequestSize{0};
  std::size_t _written{0};  // bytes of _out written
  Connection _cnx;
  EventBmp _interest{0};
  HttpClientErrc _error{HttpClientErrc::noError};
  Phase _phase{Phase::Idle};
};

}  // namespace aeronet::internal
