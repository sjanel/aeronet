// aeronet_client.cpp - scripted-client benchmark driver for aeronet's HttpClient.
//
// Drives one aeronet::HttpClient per worker thread through the shared harness. The request (method, target,
// headers, body) is built once and reused. Response decompression is enabled only for the `compress`
// scenario; otherwise Accept-Encoding is pinned to identity so the server returns raw bytes and the
// comparison stays apples-to-apples with the other drivers.

#include <string>

#include "aeronet/client-request.hpp"
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-method.hpp"
#include "bench-client-harness.hpp"

namespace {

using aeronet::bench::ClientBenchConfig;
using aeronet::bench::ScenarioSpec;

aeronet::HttpClientConfig MakeConfig(const ScenarioSpec& spec) {
  aeronet::HttpClientConfig config;
  config.withDecompression(spec.decode)  // aeronet decodes the gzip body natively (its zlib-ng codec)
      .withDefaultAcceptEncoding(spec.acceptEncoding)
      .withTcpNoDelay();
  return config;
}

class AeronetSession {
 public:
  AeronetSession(const ClientBenchConfig& cfg, const ScenarioSpec& spec)
      : _client(MakeConfig(spec)),
        _spec(spec),
        _request(spec.method == "POST" ? aeronet::http::Method::POST : aeronet::http::Method::GET,
                 cfg.baseUrl + spec.path) {
    for (const auto& [name, value] : spec.requestHeaders) {
      _request.headerAddLine(name, value);
    }
    if (spec.method == "POST") {
      _request.body(spec.body, "application/octet-stream");
    }
  }

  long doRequest() {
    if (!_spec.reuse) {
      _client.clearIdleConnections();  // force a fresh connection for the no-reuse scenario
    }
    auto res = _client.request(_request);
    if (!res) {
      return -1;
    }
    return static_cast<long>(res->bodyInMemory().size());
  }

 private:
  aeronet::HttpClient _client;
  const ScenarioSpec& _spec;
  aeronet::ClientRequest _request;  // built once, reused across requests
};

}  // namespace

AERONET_CLIENT_BENCH_MAIN(AeronetSession, "aeronet")
