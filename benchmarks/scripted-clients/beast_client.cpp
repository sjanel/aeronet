// beast_client.cpp - scripted-client benchmark driver for Boost.Beast (synchronous HTTP/1.1 over Asio).
//
// One socket per worker thread, reused across requests for the keep-alive scenarios. Beast's blocking free
// functions (connect / write / read) are driven directly, so no io_context run loop is needed. Accept-Encoding
// is pinned to identity to match the other drivers.

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <cstdint>
#include <string>
#include <string_view>

#include "bench-client-gzip.hpp"
#include "bench-client-harness.hpp"

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

using aeronet::bench::ClientBenchConfig;
using aeronet::bench::ScenarioSpec;

// Split "http://host:port" into host + port (defaults to 80 when the port is omitted).
struct HostPort {
  std::string host;
  std::string port;
};

HostPort ParseBaseUrl(std::string_view url) {
  constexpr std::string_view kScheme = "http://";
  if (url.starts_with(kScheme)) {
    url.remove_prefix(kScheme.size());
  }
  const auto slash = url.find('/');
  if (slash != std::string_view::npos) {
    url = url.substr(0, slash);
  }
  const auto colon = url.find(':');
  if (colon == std::string_view::npos) {
    return {std::string(url), "80"};
  }
  return {std::string(url.substr(0, colon)), std::string(url.substr(colon + 1))};
}

class BeastSession {
 public:
  // Boost.Beast's core speaks HTTP/1.1 only; the harness skips this driver for h2c / h2-tls.
  static constexpr bool kSupportsHttp2 = false;

  BeastSession(const ClientBenchConfig& cfg, const ScenarioSpec& spec) : _spec(spec), _socket(_ioc) {
    const HostPort hp = ParseBaseUrl(cfg.baseUrl);
    _host = hp.host;
    _endpoints = tcp::resolver(_ioc).resolve(hp.host, hp.port);

    _request.version(11);
    _request.method(spec.method == "POST" ? http::verb::post : http::verb::get);
    _request.target(spec.path);
    _request.set(http::field::host, _host);
    _request.set(http::field::accept_encoding, spec.acceptEncoding);
    for (const auto& [name, value] : spec.requestHeaders) {
      _request.set(name, value);
    }
    _request.keep_alive(spec.reuse);
    if (spec.method == "POST") {
      _request.set(http::field::content_type, "application/octet-stream");
      _request.body() = spec.body;
      _request.prepare_payload();
    }
  }

  long doRequest() {
    try {
      if (!_connected || !_spec.reuse) {
        connect();
      }
      const long bytes = exchange();
      if (!_spec.reuse) {
        close();
      }
      return bytes;
    } catch (...) {
      close();
      if (_spec.reuse) {  // a single reconnect covers server-side keep-alive churn
        try {
          connect();
          return exchange();
        } catch (...) {
          close();
        }
      }
      return -1;
    }
  }

 private:
  void connect() {
    close();
    _socket.connect(*_endpoints.begin());
    _socket.set_option(tcp::no_delay(true));
    _connected = true;
  }

  void close() {
    if (_connected) {
      beast::error_code ec;
      _socket.shutdown(tcp::socket::shutdown_both, ec);
      _socket.close(ec);
      _connected = false;
    }
  }

  long exchange() {
    http::write(_socket, _request);
    beast::flat_buffer buffer;
    http::response_parser<http::string_body> parser;
    parser.body_limit(256ULL * 1024 * 1024);
    http::read(_socket, buffer, parser);
    const std::string& body = parser.get().body();
    // Decode the gzip body with the shared zlib-ng helper (beast has no built-in content-coding).
    return _spec.decode ? aeronet::bench::GunzipDecodedSize(body) : static_cast<long>(body.size());
  }

  net::io_context _ioc;
  const ScenarioSpec& _spec;
  std::string _host;
  tcp::resolver::results_type _endpoints;
  tcp::socket _socket;
  http::request<http::string_body> _request;
  bool _connected{false};
};

}  // namespace

AERONET_CLIENT_BENCH_MAIN(BeastSession, "beast")
