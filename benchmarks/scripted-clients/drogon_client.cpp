// drogon_client.cpp - scripted-client benchmark driver for Drogon's HttpClient.
//
// Drogon's client is event-loop based; we give each worker thread its own trantor EventLoopThread and drive
// the client through its synchronous sendRequest() overload (which must be called from a thread other than
// the loop's, which is exactly our case). Accept-Encoding is pinned to identity to match the other drivers.

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <trantor/net/EventLoopThread.h>

#include <string>

#include "bench-client-gzip.hpp"
#include "bench-client-harness.hpp"

namespace {

using aeronet::bench::ClientBenchConfig;
using aeronet::bench::ScenarioSpec;

class DrogonSession {
 public:
  DrogonSession(const ClientBenchConfig& cfg, const ScenarioSpec& spec)
      : _spec(spec), _baseUrl(cfg.baseUrl), _isPost(spec.method == "POST") {
    _loopThread.run();
    _client = drogon::HttpClient::newHttpClient(_baseUrl, _loopThread.getLoop());
  }

  long doRequest() {
    if (!_spec.reuse) {
      // Fresh client (and connection) for every request in the no-reuse scenario.
      _client = drogon::HttpClient::newHttpClient(_baseUrl, _loopThread.getLoop());
    }
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setPathEncode(false);  // send the target (incl. ?query) verbatim
    req->setPath(_spec.path);
    req->setMethod(_isPost ? drogon::Post : drogon::Get);
    req->addHeader("Accept-Encoding", _spec.acceptEncoding);
    for (const auto& [name, value] : _spec.requestHeaders) {
      req->addHeader(name, value);
    }
    if (_isPost) {
      req->setContentTypeString("application/octet-stream");
      req->setBody(_spec.body);
    }
    auto [result, response] = _client->sendRequest(req, kTimeoutSeconds);
    if (result != drogon::ReqResult::Ok || !response) {
      return -1;
    }
    const auto body = response->getBody();
    // Decode the gzip body with the shared zlib-ng helper (passthrough if drogon already decoded it).
    return _spec.decode ? aeronet::bench::GunzipDecodedSize({body.data(), body.size()})
                        : static_cast<long>(body.size());
  }

 private:
  static constexpr double kTimeoutSeconds = 30.0;

  trantor::EventLoopThread _loopThread;
  drogon::HttpClientPtr _client;
  const ScenarioSpec& _spec;
  std::string _baseUrl;
  bool _isPost;
};

}  // namespace

AERONET_CLIENT_BENCH_MAIN(DrogonSession, "drogon")
