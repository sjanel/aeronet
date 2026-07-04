// curl_client.cpp - scripted-client benchmark driver for libcurl (the reference C HTTP client).
//
// One reused easy handle per worker thread (keep-alive). For the `compress` scenario libcurl is told to
// advertise gzip via a plain header (not CURLOPT_ACCEPT_ENCODING, which would auto-decode with libcurl's
// own bundled zlib): the raw gzip body is instead decoded with the shared zlib-ng helper, so every
// non-aeronet client uses the exact same codec. Other scenarios pin Accept-Encoding to identity.

#include <curl/curl.h>

#include <mutex>
#include <string>

#include "bench-client-gzip.hpp"
#include "bench-client-harness.hpp"

namespace {

using aeronet::bench::ClientBenchConfig;
using aeronet::bench::ScenarioSpec;

void EnsureGlobalInit() {
  static std::once_flag flag;
  std::call_once(flag, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

class CurlSession {
 public:
  CurlSession(const ClientBenchConfig& cfg, const ScenarioSpec& spec) : _spec(spec), _url(cfg.baseUrl + spec.path) {
    EnsureGlobalInit();
    _handle = curl_easy_init();
    _headers = curl_slist_append(_headers, ("Accept-Encoding: " + spec.acceptEncoding).c_str());
    for (const auto& [name, value] : spec.requestHeaders) {
      _headers = curl_slist_append(_headers, (name + ": " + value).c_str());
    }

    curl_easy_setopt(_handle, CURLOPT_URL, _url.c_str());
    curl_easy_setopt(_handle, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(_handle, CURLOPT_WRITEFUNCTION, &WriteCallback);
    curl_easy_setopt(_handle, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(_handle, CURLOPT_NOSIGNAL, 1L);

    // Protocol selection, mirroring the aeronet driver: HTTP/1.1, cleartext HTTP/2 (prior knowledge, no
    // Upgrade dance) or HTTP/2 over TLS (ALPN "h2"). libcurl links nghttp2 so h2 is native here.
    long httpVersion = CURL_HTTP_VERSION_1_1;
    switch (cfg.protocol) {
      case aeronet::bench::Protocol::H2c:
        httpVersion = CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE;
        break;
      case aeronet::bench::Protocol::H2Tls:
        httpVersion = CURL_HTTP_VERSION_2TLS;
        // Self-signed bench cert: skip chain + hostname verification (apples-to-apples with aeronet).
        curl_easy_setopt(_handle, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(_handle, CURLOPT_SSL_VERIFYHOST, 0L);
        break;
      case aeronet::bench::Protocol::Http1:
        break;
    }
    curl_easy_setopt(_handle, CURLOPT_HTTP_VERSION, httpVersion);
    if (!spec.reuse) {
      curl_easy_setopt(_handle, CURLOPT_FORBID_REUSE, 1L);
      curl_easy_setopt(_handle, CURLOPT_FRESH_CONNECT, 1L);
    }
    if (spec.method == "POST") {
      _headers = curl_slist_append(_headers, "Content-Type: application/octet-stream");
      curl_easy_setopt(_handle, CURLOPT_POST, 1L);
      curl_easy_setopt(_handle, CURLOPT_POSTFIELDS, _spec.body.data());
      curl_easy_setopt(_handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(_spec.body.size()));
    }
    curl_easy_setopt(_handle, CURLOPT_HTTPHEADER, _headers);
  }

  CurlSession(const CurlSession&) = delete;
  CurlSession& operator=(const CurlSession&) = delete;

  ~CurlSession() {
    if (_handle != nullptr) {
      curl_easy_cleanup(_handle);
    }
    if (_headers != nullptr) {
      curl_slist_free_all(_headers);
    }
  }

  long doRequest() {
    _received = 0;
    _body.clear();
    if (curl_easy_perform(_handle) != CURLE_OK) {
      return -1;
    }
    return _spec.decode ? aeronet::bench::GunzipDecodedSize(_body) : static_cast<long>(_received);
  }

 private:
  static std::size_t WriteCallback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* self = static_cast<CurlSession*>(userdata);
    const std::size_t received = size * nmemb;
    if (self->_spec.decode) {
      self->_body.append(ptr, received);  // accumulate raw gzip for the shared zlib-ng decode
    } else {
      self->_received += received;
    }
    return received;
  }

  CURL* _handle{nullptr};
  curl_slist* _headers{nullptr};
  const ScenarioSpec& _spec;
  std::string _url;
  std::string _body;         // raw response body (compress scenario only)
  std::size_t _received{0};  // discarded-byte counter (non-decode scenarios)
};

}  // namespace

AERONET_CLIENT_BENCH_MAIN(CurlSession, "curl")
