#pragma once

#include <glaze/glaze.hpp>
#include <map>
#include <string>
#include <string_view>

#include "aeronet/builtin-probes-config.hpp"
#include "aeronet/glaze-adapters.hpp"       // IWYU pragma: keep
#include "aeronet/glaze-enum-adapters.hpp"  // IWYU pragma: keep
#include "aeronet/http-server-config.hpp"
#include "aeronet/static-file-config.hpp"
#include "aeronet/telemetry-config.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/vector.hpp"

// ============================================================================
// TLSConfig::SniCertificate - private StaticConcatenatedStrings<5>
// ============================================================================
template <>
struct glz::meta<aeronet::TLSConfig::SniCertificate> {
  using T = aeronet::TLSConfig::SniCertificate;
  static constexpr auto value = glz::object("pattern",
                                            glz::custom<[](T& self, std::string_view sv) { self.setPattern(sv); },
                                                        [](const T& self) { return self.pattern(); }>,
                                            "certFile",
                                            glz::custom<[](T& self, std::string_view sv) { self.setCertFile(sv); },
                                                        [](const T& self) { return self.certFile(); }>,
                                            "keyFile",
                                            glz::custom<[](T& self, std::string_view sv) { self.setKeyFile(sv); },
                                                        [](const T& self) { return self.keyFile(); }>,
                                            "certPem",
                                            glz::custom<[](T& self, std::string_view sv) { self.setCertPem(sv); },
                                                        [](const T& self) { return self.certPem(); }>,
                                            "keyPem",
                                            glz::custom<[](T& self, std::string_view sv) { self.setKeyPem(sv); },
                                                        [](const T& self) { return self.keyPem(); }>,
                                            "isWildcard", &T::isWildcard);
};

// ============================================================================
// TLSConfig - private StaticConcatenatedStrings + ConcatenatedStrings32 fields
// ============================================================================
template <>
struct glz::meta<aeronet::TLSConfig> {
  using T = aeronet::TLSConfig;
  static constexpr auto value =
      glz::object(
          "enabled", &T::enabled, "requestClientCert", &T::requestClientCert, "requireClientCert",
          &T::requireClientCert, "alpnMustMatch", &T::alpnMustMatch, "logHandshake", &T::logHandshake,
          "disableCompression", &T::disableCompression, "cipherPolicy", &T::cipherPolicy, "ktlsMode", &T::ktlsMode,
          "minVersion", &T::minVersion, "maxVersion", &T::maxVersion, "maxConcurrentHandshakes",
          &T::maxConcurrentHandshakes, "handshakeRateLimitPerSecond", &T::handshakeRateLimitPerSecond,
          "handshakeRateLimitBurst", &T::handshakeRateLimitBurst, "handshakeTimeout", &T::handshakeTimeout,
          "sessionTickets", &T::sessionTickets, "certFile",
          glz::custom<[](T& self, const std::string& str) { self.withCertFile(str); },
                      [](const T& self) { return self.certFile(); }>,
          "keyFile",
          glz::custom<[](T& self, const std::string& str) { self.withKeyFile(str); },
                      [](const T& self) { return self.keyFile(); }>,
          "certPem",
          glz::custom<[](T& self, const std::string& str) { self.withCertPem(str); },
                      [](const T& self) { return self.certPem(); }>,
          "keyPem",
          glz::custom<[](T& self, const std::string& str) { self.withKeyPem(str); },
                      [](const T& self) { return self.keyPem(); }>,
          "cipherList",
          glz::custom<[](T& self, const std::string& str) { self.withCipherList(str); },
                      [](const T& self) { return self.cipherList(); }>,
          "alpnProtocols",
          glz::custom<[](T& self, const ::aeronet::vector<std::string>& protos) { self.withTlsAlpnProtocols(protos); },
                      [](const T& self) {
                        ::aeronet::vector<std::string_view> result;
                        for (auto sv : self.alpnProtocols()) {
                          result.push_back(sv);
                        }
                        return result;
                      }>,
          "trustedClientCertsPem",
          glz::custom<
              [](T& self, const ::aeronet::vector<std::string>& certs) {
                self.withoutTlsTrustedClientCert();
                for (const auto& cert : certs) {
                  self.withTlsTrustedClientCert(cert);
                }
              },
              [](const T& self) {
                ::aeronet::vector<std::string_view> result;
                for (auto sv : self.trustedClientCertsPem()) {
                  result.push_back(sv);
                }
                return result;
              }>,
          "sniCertificates",
          glz::custom<[](T& self, const ::aeronet::vector<aeronet::TLSConfig::SniCertificate>& certs) {
            self.clearTlsSniCertificates();
            for (const auto& cert : certs) {
              if (cert.hasFiles()) {
                self.withTlsSniCertificateFiles(cert.pattern(), cert.certFile(), cert.keyFile());
              } else if (cert.hasPem()) {
                self.withTlsSniCertificateMemory(cert.pattern(), cert.certPem(), cert.keyPem());
              }
            }
          },
                      [](const T& self) { return self.sniCertificates(); }>);
};

// ============================================================================
// TelemetryConfig - private StaticConcatenatedStrings + custom handling for tags/headers/buckets
// ============================================================================
template <>
struct glz::meta<aeronet::TelemetryConfig> {
  using T = aeronet::TelemetryConfig;
  static constexpr auto value =
      glz::object("otelEnabled", &T::otelEnabled, "dogStatsDEnabled", &T::dogStatsDEnabled, "sampleRate",
                  &T::sampleRate, "exportInterval", &T::exportInterval, "exportTimeout", &T::exportTimeout, "endpoint",
                  glz::custom<[](T& self, std::string_view sv) { self.withEndpoint(sv); },
                              [](const T& self) { return self.endpoint(); }>,
                  "serviceName",
                  glz::custom<[](T& self, std::string_view sv) { self.withServiceName(sv); },
                              [](const T& self) { return self.serviceName(); }>,
                  "dogstatsdSocketPath",
                  glz::custom<[](T& self, std::string_view sv) { self.withDogStatsdSocketPath(sv); },
                              [](const T& self) { return self.dogstatsdSocketPath(); }>,
                  "dogstatsdNamespace",
                  glz::custom<[](T& self, std::string_view sv) { self.withDogStatsdNamespace(sv); },
                              [](const T& self) { return self.dogstatsdNamespace(); }>,
                  // Tags as array of "key:value" strings
                  "dogstatsdTags",
                  glz::custom<[](T& self, const ::aeronet::vector<std::string>& tags) {
                    for (const auto& tag : tags) {
                      self.addDogStatsdTag(tag);
                    }
                  },
                              [](const T& self) {
                                ::aeronet::vector<std::string_view> result;
                                for (auto sv : self.dogstatsdTags()) {
                                  result.push_back(sv);
                                }
                                return result;
                              }>,
                  // HTTP headers as array of "name:value" strings
                  "httpHeaders",
                  glz::custom<
                      [](T& self, const ::aeronet::vector<std::string>& headers) {
                        for (const auto& hdr : headers) {
                          if (auto sep = hdr.find(':'); sep != std::string::npos) {
                            self.addHttpHeader(hdr.substr(0, sep), hdr.substr(sep + 1));
                          }
                        }
                      },
                      [](const T& self) {
                        ::aeronet::vector<std::string_view> result;
                        for (auto sv : self.httpHeadersRange()) {
                          result.push_back(sv);
                        }
                        return result;
                      }>,
                  // Histogram buckets as JSON object {"metric_name": [1.0, 5.0, 10.0]}
                  "histogramBuckets",
                  glz::custom<[](T& self, std::map<std::string, ::aeronet::vector<double>> buckets) {
                    for (auto& [name, boundaries] : buckets) {
                      self.addHistogramBuckets(name, boundaries);
                    }
                  },
                              [](const T& self) {
                                std::map<std::string, ::aeronet::vector<double>> result;
                                for (const auto& [key, boundaries] : self.histogramBuckets()) {
                                  result[std::string(std::string_view(key))] =
                                      ::aeronet::vector<double>(boundaries.begin(), boundaries.end());
                                }
                                return result;
                              }>);
};

// ============================================================================
// StaticFileConfig - private StaticConcatenatedStrings, skip non-serializable std::function fields
// ============================================================================
template <>
struct glz::meta<aeronet::StaticFileConfig> {
  using T = aeronet::StaticFileConfig;
  static constexpr auto value =
      glz::object("enableRange", &T::enableRange, "maxMultipartRanges", &T::maxMultipartRanges, "maxMultipartBodySize",
                  &T::maxMultipartBodySize, "enableConditional", &T::enableConditional, "addLastModified",
                  &T::addLastModified, "addEtag", &T::addEtag, "enableDirectoryIndex", &T::enableDirectoryIndex,
                  "showHiddenFiles", &T::showHiddenFiles, "inlineFileThresholdBytes", &T::inlineFileThresholdBytes,
                  "maxEntriesToList", &T::maxEntriesToList, "defaultIndex",
                  glz::custom<[](T& self, std::string_view sv) { self.withDefaultIndex(sv); },
                              [](const T& self) { return self.defaultIndex(); }>,
                  "defaultContentType",
                  glz::custom<[](T& self, std::string_view sv) { self.withDefaultContentType(sv); },
                              [](const T& self) { return self.defaultContentType(); }>,
                  "directoryListingCss",
                  glz::custom<[](T& self, std::string_view sv) { self.withDirectoryListingCss(sv); },
                              [](const T& self) { return self.directoryListingCss(); }>);
};
// ============================================================================
// BuiltinProbesConfig - private StaticConcatenatedStrings, exposed as named fields
// ============================================================================
template <>
struct glz::meta<aeronet::BuiltinProbesConfig> {
  using T = aeronet::BuiltinProbesConfig;
  static constexpr auto value =
      glz::object("enabled", &T::enabled, "contentType", &T::contentType, "livenessPath",
                  glz::custom<[](T& self, const std::string& path) { self.withLivenessPath(path); },
                              [](const T& self) { return self.livenessPath(); }>,
                  "readinessPath",
                  glz::custom<[](T& self, const std::string& path) { self.withReadinessPath(path); },
                              [](const T& self) { return self.readinessPath(); }>,
                  "startupPath",
                  glz::custom<[](T& self, const std::string& path) { self.withStartupPath(path); },
                              [](const T& self) { return self.startupPath(); }>);
};

// ============================================================================
// HttpServerConfig - private _connectAllowlist needs custom lambda
// ============================================================================
template <>
struct glz::meta<aeronet::HttpServerConfig> {
  using T = aeronet::HttpServerConfig;
  static constexpr auto value = glz::object(
      "nbThreads", &T::nbThreads, "port", &T::port, "reusePort", &T::reusePort, "tcpNoDelay", &T::tcpNoDelay,
      "enableKeepAlive", &T::enableKeepAlive, "mergeUnknownRequestHeaders", &T::mergeUnknownRequestHeaders,
      "traceMethodPolicy", &T::traceMethodPolicy, "addTrailerHeader", &T::addTrailerHeader, "zerocopyMode",
      &T::zerocopyMode, "zerocopyMinBytes", &T::zerocopyMinBytes, "maxRequestsPerConnection",
      &T::maxRequestsPerConnection, "maxCachedConnections", &T::maxCachedConnections, "keepAliveTimeout",
      &T::keepAliveTimeout, "maxHeaderBytes", &T::maxHeaderBytes, "maxBodyBytes", &T::maxBodyBytes,
      "minCapturedBodySize", &T::minCapturedBodySize, "maxOutboundBufferBytes", &T::maxOutboundBufferBytes,
      "pollInterval", &T::pollInterval, "pollIntervalMinFactor", &T::pollIntervalMinFactor, "pollIntervalMaxFactor",
      &T::pollIntervalMaxFactor, "headerReadTimeout", &T::headerReadTimeout, "bodyReadTimeout", &T::bodyReadTimeout,
      "tls", &T::tls,
#ifdef AERONET_ENABLE_HTTP2
      "http2", &T::http2,
#endif
      "telemetry", &T::telemetry, "compression", &T::compression, "decompression", &T::decompression,
      "minReadChunkBytes", &T::minReadChunkBytes, "maxPerEventReadBytes", &T::maxPerEventReadBytes, "globalHeaders",
      &T::globalHeaders, "builtinProbes", &T::builtinProbes, "connectAllowlist",
      glz::custom<[](T& self, const aeronet::vector<std::string>& hosts) {
        self.withConnectAllowlist(hosts.begin(), hosts.end());
      },
                  [](const T& self) {
                    aeronet::vector<std::string_view> result;
                    for (auto sv : self.connectAllowlist()) {
                      result.push_back(sv);
                    }
                    return result;
                  }>);
};
