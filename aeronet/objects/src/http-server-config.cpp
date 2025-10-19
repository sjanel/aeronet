#include "aeronet/http-server-config.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/compression-config.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/tls-config.hpp"
#include "invalid_argument_exception.hpp"
#include "major-minor-version.hpp"
#include "tchars.hpp"

namespace aeronet {

TLSConfig& HttpServerConfig::ensureTls() {
  tls.enabled = true;
  return tls;
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
  tlsCfg.withCertFile(certFile);
  tlsCfg.withKeyFile(keyFile);
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsCipherList(std::string_view cipherList) {
  ensureTls().withCipherList(cipherList);
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsMinVersion(std::string_view ver) {
  parseVersion(ver.data(), ver.data() + ver.size(), ensureTls().minVersion);
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsMaxVersion(std::string_view ver) {
  parseVersion(ver.data(), ver.data() + ver.size(), ensureTls().maxVersion);
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsCertKeyMemory(std::string_view certPem, std::string_view keyPem) {
  auto& tlsCfg = ensureTls();
  tlsCfg.withCertFile({});
  tlsCfg.withKeyFile({});
  tlsCfg.withCertPem(certPem);
  tlsCfg.withKeyPem(keyPem);
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
  tls.enabled = false;
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

// Set the OpenTelemetry configuration for this server instance
HttpServerConfig& HttpServerConfig::withOtelConfig(OtelConfig cfg) {
  otel = std::move(cfg);
  return *this;
}

// Configure adaptive read chunk sizing (two tier). Returns *this.
HttpServerConfig& HttpServerConfig::withReadChunkStrategy(std::size_t initialBytes, std::size_t bodyBytes) {
  initialReadChunkBytes = initialBytes;
  bodyReadChunkBytes = bodyBytes;
  return *this;
}

// Configure a per-event read fairness cap (0 => unlimited)
HttpServerConfig& HttpServerConfig::withMaxPerEventReadBytes(std::size_t capBytes) {
  maxPerEventReadBytes = capBytes;
  return *this;
}

HttpServerConfig& HttpServerConfig::withMinCapturedBodySize(std::size_t bytes) {
  this->minCapturedBodySize = bytes;
  return *this;
}

HttpServerConfig& HttpServerConfig::withRouterConfig(RouterConfig cfg) {
  router = std::move(cfg);
  return *this;
}

HttpServerConfig& HttpServerConfig::withGlobalHeaders(std::vector<http::Header> headers) {
  globalHeaders = std::move(headers);
  return *this;
}

HttpServerConfig& HttpServerConfig::withGlobalHeader(http::Header header) {
  globalHeaders.emplace_back(std::move(header));
  return *this;
}

HttpServerConfig& HttpServerConfig::withTracePolicy(TraceMethodPolicy policy) {
  traceMethodPolicy = policy;
  return *this;
}

// Enable and configure builtin probes
HttpServerConfig& HttpServerConfig::withBuiltinProbes(BuiltinProbesConfig cfg) {
  builtinProbes = std::move(cfg);
  builtinProbes.enabled = true;
  return *this;
}

HttpServerConfig& HttpServerConfig::enableBuiltinProbes(bool on) {
  builtinProbes.enabled = on;
  return *this;
}

void HttpServerConfig::validate() const {
  compression.validate();
  requestDecompression.validate();

  if (maxPerEventReadBytes != 0 && maxPerEventReadBytes < initialReadChunkBytes) {
    // Normalize: cap cannot be smaller than a single chunk; promote to chunk size.
    // (Since config is const here we cannot mutate; just throw to surface mistake.)
    throw invalid_argument("maxPerEventReadBytes must be 0 or >= initialReadChunkBytes");
  }
  for (const auto& [headerKey, headerValue] : globalHeaders) {
    if (http::IsReservedResponseHeader(headerKey)) {
      throw invalid_argument("'{}' is a reserved header", headerKey);
    }
    if (headerKey.empty() || std::ranges::any_of(headerKey, [](char ch) { return !is_tchar(ch); })) {
      throw invalid_argument("header '{}' is invalid", headerKey);
    }
    // basic sanity on header value
    if (std::ranges::any_of(headerValue, [](unsigned char ch) { return ch <= 0x1F || ch == 0x7F; })) {
      throw invalid_argument("header '{}' has invalid value characters", headerKey);
    }
  }
  otel.validate();
  tls.validate();
  builtinProbes.validate();

  // Validate some header/body limits
  if (std::cmp_less(maxHeaderBytes, 128)) {
    throw invalid_argument("maxHeaderBytes must be >= 128");
  }
  if (maxBodyBytes == 0) {
    throw invalid_argument("maxBodyBytes must be > 0");
  }
  if (keepAliveTimeout.count() < 0) {
    throw invalid_argument("keepAliveTimeout must be non-negative");
  }
  if (pollInterval.count() <= 0) {
    throw invalid_argument("pollInterval must be > 0");
  }
  if (headerReadTimeout.count() < 0) {
    throw invalid_argument("headerReadTimeout must be non-negative");
  }
  if (std::cmp_less(maxOutboundBufferBytes, 1024)) {
    throw invalid_argument("maxOutboundBufferBytes must be >= 1024");
  }
}

}  // namespace aeronet