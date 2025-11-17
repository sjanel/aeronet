#include "aeronet/http-server-config.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "aeronet/builtin-probes-config.hpp"
#include "aeronet/compression-config.hpp"
#include "aeronet/decompression-config.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/log.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/tchars.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/tls-config.hpp"

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

HttpServerConfig& HttpServerConfig::withTcpNoDelay(bool on) {
  this->tcpNoDelay = on;
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
  ensureTls().withTlsMinVersion(ver);
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsMaxVersion(std::string_view ver) {
  ensureTls().withTlsMaxVersion(ver);
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

HttpServerConfig& HttpServerConfig::withTlsKtlsMode(TLSConfig::KtlsMode mode) {
  ensureTls().ktlsMode = mode;
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
  ensureTls().handshakeTimeout = timeout;
  return *this;
}

HttpServerConfig& HttpServerConfig::withTlsTrustedClientCert(std::string_view certPem) {
  ensureTls().withTlsTrustedClientCert(certPem);
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
  decompression = std::move(cfg);
  return *this;
}

HttpServerConfig& HttpServerConfig::withMergeUnknownRequestHeaders(bool on) {
  mergeUnknownRequestHeaders = on;
  return *this;
}

// Set the telemetry configuration for this server instance
HttpServerConfig& HttpServerConfig::withTelemetryConfig(TelemetryConfig cfg) {
  telemetry = std::move(cfg);
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

HttpServerConfig& HttpServerConfig::withGlobalHeaders(std::span<const http::Header> headers) {
  if (headers.size() > kMaxGlobalHeaders) {
    throw std::invalid_argument("too many global headers");
  }
  RawChars data;
  for (const auto& header : headers) {
    data.assign(header.name);
    data.append(http::HeaderSep);
    data.append(header.value);
    globalHeaders.append(data);
  }
  return *this;
}

HttpServerConfig& HttpServerConfig::withGlobalHeader(const http::Header& header) {
  RawChars data(header.name.size() + http::HeaderSep.size() + header.value.size());
  data.unchecked_append(header.name);
  data.unchecked_append(http::HeaderSep);
  data.unchecked_append(header.value);
  globalHeaders.append(data);
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

void HttpServerConfig::validate() {
  compression.validate();
  decompression.validate();

  if (maxPerEventReadBytes != 0 && maxPerEventReadBytes < initialReadChunkBytes) {
    throw std::invalid_argument("maxPerEventReadBytes must be 0 or >= initialReadChunkBytes");
  }

  if (globalHeaders.empty()) {
    globalHeaders.append("Server: aeronet");
  } else if (globalHeaders.size() > kMaxGlobalHeaders) {
    throw std::invalid_argument("too many global headers");
  }

  for (std::string_view headerKeyValue : globalHeaders) {
    auto colonPos = headerKeyValue.find(':');
    if (colonPos == std::string_view::npos) {
      throw std::invalid_argument("header missing ':' separator in global headers");
    }

    std::string_view headerKey = headerKeyValue.substr(0, colonPos);
    std::string_view headerValue = headerKeyValue.substr(colonPos + 1);

    if (http::IsReservedResponseHeader(headerKey)) {
      log::critical("'{}' is a reserved header", headerKey);
      throw std::invalid_argument("attempt to set reserved header");
    }

    // A header key cannot have a space
    if (headerKey.empty() || std::ranges::any_of(headerKey, [](char ch) { return !is_tchar(ch); })) {
      log::critical("header '{}' is invalid", headerKey);
      throw std::invalid_argument("header has invalid key");
    }

    // basic sanity on header value
    if (std::ranges::any_of(headerValue, [](unsigned char ch) { return ch <= 0x1F || ch == 0x7F; })) {
      log::critical("header '{}' has invalid value characters", headerKey);
      throw std::invalid_argument("header has invalid value");
    }
  }

  telemetry.validate();
  tls.validate();
  builtinProbes.validate();

  // Validate some header/body limits
  if (std::cmp_less(maxHeaderBytes, 128)) {
    throw std::invalid_argument("maxHeaderBytes must be >= 128");
  }
  if (maxBodyBytes == 0) {
    throw std::invalid_argument("maxBodyBytes must be > 0");
  }
  if (keepAliveTimeout.count() < 0) {
    throw std::invalid_argument("keepAliveTimeout must be non-negative");
  }
  if (pollInterval.count() <= 0) {
    throw std::invalid_argument("pollInterval must be > 0");
  }
  if (headerReadTimeout.count() < 0) {
    throw std::invalid_argument("headerReadTimeout must be non-negative");
  }
  if (std::cmp_less(maxOutboundBufferBytes, 1024)) {
    throw std::invalid_argument("maxOutboundBufferBytes must be >= 1024");
  }
}

}  // namespace aeronet