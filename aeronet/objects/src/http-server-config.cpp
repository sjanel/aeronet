#include "aeronet/http-server-config.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/tls-config.hpp"

namespace aeronet {

TLSConfig& HttpServerConfig::ensureTls() {
  if (!tls) {
    tls.emplace();
  }
  return *tls;
}

HttpServerConfig& HttpServerConfig::withPort(uint16_t port) {
  this->port = port;
  return *this;
}

HttpServerConfig& HttpServerConfig::withReusePort(bool on) {
  this->reusePort = on;
  return *this;
}

HttpServerConfig& HttpServerConfig::withKeepAliveMode(bool on) {
  this->enableKeepAlive = on;
  return *this;
}

HttpServerConfig& HttpServerConfig::withMaxHeaderBytes(std::size_t maxHeaderBytes) {
  this->maxHeaderBytes = maxHeaderBytes;
  return *this;
}

HttpServerConfig& HttpServerConfig::withMaxBodyBytes(std::size_t maxBodyBytes) {
  this->maxBodyBytes = maxBodyBytes;
  return *this;
}

HttpServerConfig& HttpServerConfig::withMaxOutboundBufferBytes(std::size_t maxOutbound) {
  this->maxOutboundBufferBytes = maxOutbound;
  return *this;
}

HttpServerConfig& HttpServerConfig::withMaxRequestsPerConnection(uint32_t maxRequests) {
  this->maxRequestsPerConnection = maxRequests;
  return *this;
}

HttpServerConfig& HttpServerConfig::withKeepAliveTimeout(std::chrono::milliseconds timeout) {
  this->keepAliveTimeout = timeout;
  return *this;
}

HttpServerConfig& HttpServerConfig::withPollInterval(std::chrono::milliseconds interval) {
  this->pollInterval = interval;
  return *this;
}

HttpServerConfig& HttpServerConfig::withHeaderReadTimeout(std::chrono::milliseconds timeout) {
  this->headerReadTimeout = timeout;
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsCertKey(std::string_view certFile, std::string_view keyFile) {
  auto& tlsCfg = ensureTls();
  tlsCfg.certFile.assign(certFile);
  tlsCfg.keyFile.assign(keyFile);
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsCipherList(std::string_view cipherList) {
  ensureTls().cipherList.assign(cipherList);
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsMinVersion(std::string_view ver) {
  ensureTls().minVersion.assign(ver);
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsMaxVersion(std::string_view ver) {
  ensureTls().maxVersion.assign(ver);
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsCertKeyMemory(std::string_view certPem, std::string_view keyPem) {
  auto& tlsCfg = ensureTls();
  tlsCfg.certFile.clear();
  tlsCfg.keyFile.clear();
  tlsCfg.certPem.assign(certPem);
  tlsCfg.keyPem.assign(keyPem);
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsRequestClientCert(bool on) {
  ensureTls().requestClientCert = on;
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsRequireClientCert(bool on) {
  auto& tlsCfg = ensureTls();
  tlsCfg.requireClientCert = on;
  if (on) {
    tlsCfg.requestClientCert = true;  // logical implication
  }
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsAlpnMustMatch(bool on) {
  ensureTls().alpnMustMatch = on;
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsHandshakeLogging(bool on) {
  ensureTls().logHandshake = on;
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsHandshakeTimeout(std::chrono::milliseconds timeout) {
  tlsHandshakeTimeout = timeout;
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsAddTrustedClientCert(std::string_view certPem) {
  ensureTls().trustedClientCertsPem.emplace_back(certPem);
  return *this;
}

HttpServerConfig& HttpServerConfig::withoutTls() {
  tls.reset();
  return *this;
}

HttpServerConfig& HttpServerConfig::withTrailingSlashPolicy(TrailingSlashPolicy policy) {
  trailingSlashPolicy = policy;
  return *this;
}

// Enable / configure response compression. Passing by value allows caller to move.
HttpServerConfig& HttpServerConfig::withCompression(CompressionConfig cfg) {
  compression = std::move(cfg);
  return *this;
}

HttpServerConfig& HttpServerConfig::withRequestDecompression(DecompressionConfig cfg) {
  requestDecompression = std::move(cfg);
  return *this;
}

HttpServerConfig& HttpServerConfig::withMergeUnknownRequestHeaders(bool on) {
  mergeUnknownRequestHeaders = on;
  return *this;
}

void HttpServerConfig::validate() const { compression.validate(); }

}  // namespace aeronet