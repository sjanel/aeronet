#include "aeronet/config-loader.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aeronet/aeronet-config.hpp"
#include "aeronet/http-response.hpp"
#include "aeronet/multi-http-server.hpp"
#include "aeronet/router.hpp"
#include "aeronet/single-http-server.hpp"

namespace aeronet {

// ============================================================================
// Duration string parsing (via round-trip)
// ============================================================================
TEST(ConfigLoaderTest, DurationMillisecondsInteger) {
  auto config = detail::ParseConfigString(R"({"server":{"keepAliveTimeout":3000}})", ConfigFormat::json);
  EXPECT_EQ(config.server.keepAliveTimeout, std::chrono::milliseconds{3000});
}

TEST(ConfigLoaderTest, DurationMillisecondsString) {
  auto config = detail::ParseConfigString(R"({"server":{"keepAliveTimeout":"5s"}})", ConfigFormat::json);
  EXPECT_EQ(config.server.keepAliveTimeout, std::chrono::milliseconds{5000});
}

TEST(ConfigLoaderTest, DurationCompoundString) {
  auto config = detail::ParseConfigString(R"({"server":{"keepAliveTimeout":"1m30s"}})", ConfigFormat::json);
  EXPECT_EQ(config.server.keepAliveTimeout, std::chrono::milliseconds{90000});
}

TEST(ConfigLoaderTest, DurationStringWithSpaces) {
  auto config = detail::ParseConfigString(R"({"server":{"keepAliveTimeout":"  2s  "}})", ConfigFormat::json);
  EXPECT_EQ(config.server.keepAliveTimeout, std::chrono::milliseconds{2000});
}

TEST(ConfigLoaderTest, DurationSubMillisecond) {
  EXPECT_THROW(detail::ParseConfigString(R"({"server":{"keepAliveTimeout":"500us"}})", ConfigFormat::json),
               std::runtime_error);
}

TEST(ConfigLoaderTest, DurationHours) {
  auto config = detail::ParseConfigString(R"({"server":{"keepAliveTimeout":"1h"}})", ConfigFormat::json);
  EXPECT_EQ(config.server.keepAliveTimeout, std::chrono::milliseconds{3600000});
}

TEST(ConfigLoaderTest, DurationInvalidString) {
  EXPECT_THROW(detail::ParseConfigString(R"({"server":{"keepAliveTimeout":"invalid"}})", ConfigFormat::json),
               std::runtime_error);
}

TEST(ConfigLoaderTest, DurationEmptyString) {
  EXPECT_THROW(detail::ParseConfigString(R"({"server":{"keepAliveTimeout":""}})", ConfigFormat::json),
               std::runtime_error);
}

// ============================================================================
// Default values preservation
// ============================================================================
TEST(ConfigLoaderTest, EmptyJsonPreservesDefaults) {
  auto config = detail::ParseConfigString("{}", ConfigFormat::json);
  HttpServerConfig defaults;
  EXPECT_EQ(config.server.nbThreads, defaults.nbThreads);
  EXPECT_EQ(config.server.port, defaults.port);
  EXPECT_EQ(config.server.reusePort, defaults.reusePort);
  EXPECT_EQ(config.server.enableKeepAlive, defaults.enableKeepAlive);
  EXPECT_EQ(config.server.maxHeaderBytes, defaults.maxHeaderBytes);
  EXPECT_EQ(config.server.maxBodyBytes, defaults.maxBodyBytes);
  EXPECT_EQ(config.server.keepAliveTimeout, defaults.keepAliveTimeout);
  EXPECT_EQ(config.server.tcpNoDelay, defaults.tcpNoDelay);
  EXPECT_EQ(config.server.zerocopyMode, defaults.zerocopyMode);
}

// ============================================================================
// Basic server fields
// ============================================================================
TEST(ConfigLoaderTest, BasicServerFields) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "nbThreads": 4,
      "port": 8080,
      "reusePort": true,
      "enableKeepAlive": false,
      "maxHeaderBytes": 16384,
      "maxBodyBytes": 1048576,
      "maxRequestsPerConnection": 500,
      "maxCachedConnections": 20,
      "minReadChunkBytes": 8192,
      "maxPerEventReadBytes": 65536,
      "zerocopyMinBytes": 4096,
      "minCapturedBodySize": 2048,
      "maxOutboundBufferBytes": 8388608,
      "mergeUnknownRequestHeaders": false,
      "addTrailerHeader": false
    }
  })",
                                          ConfigFormat::json);
  EXPECT_EQ(config.server.nbThreads, 4U);
  EXPECT_EQ(config.server.port, 8080U);
  EXPECT_TRUE(config.server.reusePort);
  EXPECT_FALSE(config.server.enableKeepAlive);
  EXPECT_EQ(config.server.maxHeaderBytes, 16384U);
  EXPECT_EQ(config.server.maxBodyBytes, 1048576U);
  EXPECT_EQ(config.server.maxRequestsPerConnection, 500U);
  EXPECT_EQ(config.server.maxCachedConnections, 20U);
  EXPECT_EQ(config.server.minReadChunkBytes, 8192U);
  EXPECT_EQ(config.server.maxPerEventReadBytes, 65536U);
  EXPECT_EQ(config.server.zerocopyMinBytes, 4096U);
  EXPECT_EQ(config.server.minCapturedBodySize, 2048U);
  EXPECT_EQ(config.server.maxOutboundBufferBytes, 8388608U);
  EXPECT_FALSE(config.server.mergeUnknownRequestHeaders);
  EXPECT_FALSE(config.server.addTrailerHeader);
}

// ============================================================================
// Enum fields
// ============================================================================
TEST(ConfigLoaderTest, TcpNoDelayEnum) {
  auto config = detail::ParseConfigString(R"({"server":{"tcpNoDelay":"enabled"}})", ConfigFormat::json);
  EXPECT_EQ(config.server.tcpNoDelay, TcpNoDelayMode::Enabled);
}

TEST(ConfigLoaderTest, ZerocopyModeEnum) {
  auto config = detail::ParseConfigString(R"({"server":{"zerocopyMode":"disabled"}})", ConfigFormat::json);
  EXPECT_EQ(config.server.zerocopyMode, ZerocopyMode::Disabled);
}

TEST(ConfigLoaderTest, TraceMethodPolicyEnum) {
  auto config = detail::ParseConfigString(R"({"server":{"traceMethodPolicy":"enabledPlainOnly"}})", ConfigFormat::json);
  EXPECT_EQ(config.server.traceMethodPolicy, HttpServerConfig::TraceMethodPolicy::EnabledPlainOnly);
}

// ============================================================================
// TLS configuration
// ============================================================================
TEST(ConfigLoaderTest, TlsBasicFields) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "tls": {
        "enabled": true,
        "certFile": "/etc/tls/tls.crt",
        "keyFile": "/etc/tls/tls.key",
        "cipherPolicy": "modern",
        "ktlsMode": "enabled",
        "minVersion": "1.2",
        "maxVersion": "1.3",
        "disableCompression": false,
        "requestClientCert": true,
        "requireClientCert": false,
        "logHandshake": true,
        "maxConcurrentHandshakes": 100,
        "handshakeRateLimitPerSecond": 50,
        "handshakeRateLimitBurst": 10,
        "handshakeTimeout": "10s"
      }
    }
  })",
                                          ConfigFormat::json);
  EXPECT_TRUE(config.server.tls.enabled);
  EXPECT_EQ(config.server.tls.certFile(), "/etc/tls/tls.crt");
  EXPECT_EQ(config.server.tls.keyFile(), "/etc/tls/tls.key");
  EXPECT_EQ(config.server.tls.cipherPolicy, TLSConfig::CipherPolicy::Modern);
  EXPECT_EQ(config.server.tls.ktlsMode, TLSConfig::KtlsMode::Enabled);
  EXPECT_TRUE(config.server.tls.minVersion.isValid());
  EXPECT_EQ(config.server.tls.minVersion.major(), 1);
  EXPECT_EQ(config.server.tls.minVersion.minor(), 2);
  EXPECT_TRUE(config.server.tls.maxVersion.isValid());
  EXPECT_EQ(config.server.tls.maxVersion.major(), 1);
  EXPECT_EQ(config.server.tls.maxVersion.minor(), 3);
  EXPECT_FALSE(config.server.tls.disableCompression);
  EXPECT_TRUE(config.server.tls.requestClientCert);
  EXPECT_FALSE(config.server.tls.requireClientCert);
  EXPECT_FALSE(config.server.tls.alpnMustMatch);
  EXPECT_TRUE(config.server.tls.logHandshake);
  EXPECT_EQ(config.server.tls.maxConcurrentHandshakes, 100U);
  EXPECT_EQ(config.server.tls.handshakeRateLimitPerSecond, 50U);
  EXPECT_EQ(config.server.tls.handshakeRateLimitBurst, 10U);
  EXPECT_EQ(config.server.tls.handshakeTimeout, std::chrono::milliseconds{10000});
}

TEST(ConfigLoaderTest, TlsPemFields) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "tls": {
        "enabled": true,
        "certPem": "-----BEGIN CERTIFICATE-----\ntest\n-----END CERTIFICATE-----",
        "keyPem": "-----BEGIN PRIVATE KEY-----\ntest\n-----END PRIVATE KEY-----",
        "cipherList": "TLS_AES_256_GCM_SHA384"
      }
    }
  })",
                                          ConfigFormat::json);
  EXPECT_EQ(config.server.tls.certPem(), "-----BEGIN CERTIFICATE-----\ntest\n-----END CERTIFICATE-----");
  EXPECT_EQ(config.server.tls.keyPem(), "-----BEGIN PRIVATE KEY-----\ntest\n-----END PRIVATE KEY-----");
  EXPECT_EQ(config.server.tls.cipherList(), "TLS_AES_256_GCM_SHA384");
}

TEST(ConfigLoaderTest, TlsSessionTickets) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "tls": {
        "sessionTickets": {
          "enabled": true,
          "maxKeys": 4,
          "lifetime": 7200
        }
      }
    }
  })",
                                          ConfigFormat::json);
  EXPECT_TRUE(config.server.tls.sessionTickets.enabled);
  EXPECT_EQ(config.server.tls.sessionTickets.maxKeys, 4U);
  EXPECT_EQ(config.server.tls.sessionTickets.lifetime, std::chrono::seconds{7200});
}

TEST(ConfigLoaderTest, TlsAlpnProtocols) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "tls": {
        "alpnProtocols": ["h2", "http/1.1"]
      }
    }
  })",
                                          ConfigFormat::json);
  std::vector<std::string_view> protos;
  for (auto sv : config.server.tls.alpnProtocols()) {
    protos.push_back(sv);
  }
  ASSERT_EQ(protos.size(), 2U);
  EXPECT_EQ(protos[0], "h2");
  EXPECT_EQ(protos[1], "http/1.1");
}

TEST(ConfigLoaderTest, TlsTrustedClientCerts) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "tls": {
        "trustedClientCertsPem": ["cert1-pem", "cert2-pem"]
      }
    }
  })",
                                          ConfigFormat::json);
  std::vector<std::string_view> certs;
  for (auto sv : config.server.tls.trustedClientCertsPem()) {
    certs.push_back(sv);
  }
  ASSERT_EQ(certs.size(), 2U);
  EXPECT_EQ(certs[0], "cert1-pem");
  EXPECT_EQ(certs[1], "cert2-pem");
}

TEST(ConfigLoaderTest, TlsSniCertificates) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "tls": {
        "enabled": true,
        "certFile": "default.crt",
        "keyFile": "default.key",
        "sniCertificates": [
          {"pattern": "example.com", "certFile": "/certs/example.crt", "keyFile": "/certs/example.key"},
          {"pattern": "other.com", "certPem": "pem-cert", "keyPem": "pem-key"}
        ]
      }
    }
  })",
                                          ConfigFormat::json);
  auto sniCerts = config.server.tls.sniCertificates();
  ASSERT_EQ(sniCerts.size(), 2U);
  EXPECT_EQ(sniCerts[0].pattern(), "example.com");
  EXPECT_EQ(sniCerts[0].certFile(), "/certs/example.crt");
  EXPECT_EQ(sniCerts[0].keyFile(), "/certs/example.key");
  EXPECT_EQ(sniCerts[1].pattern(), "other.com");
  EXPECT_EQ(sniCerts[1].certPem(), "pem-cert");
  EXPECT_EQ(sniCerts[1].keyPem(), "pem-key");
}

// ============================================================================
// HTTP/2 configuration
// ============================================================================
#ifdef AERONET_ENABLE_HTTP2
TEST(ConfigLoaderTest, Http2Config) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "http2": {
        "enable": true,
        "enablePush": true,
        "enableH2c": false,
        "enableH2cUpgrade": false,
        "maxConcurrentStreams": 256,
        "initialWindowSize": 131070,
        "maxFrameSize": 32768,
        "maxHeaderListSize": 16384,
        "connectionWindowSize": 2097152,
        "settingsTimeout": "3s",
        "pingInterval": "30s",
        "pingTimeout": "5s",
        "maxStreamsPerConnection": 1000,
        "maxPriorityTreeDepth": 128,
        "headerTableSize": 8192,
        "enablePriority": false,
        "mergeUnknownRequestHeaders": false
      }
    }
  })",
                                          ConfigFormat::json);
  EXPECT_TRUE(config.server.http2.enable);
  EXPECT_TRUE(config.server.http2.enablePush);
  EXPECT_FALSE(config.server.http2.enableH2c);
  EXPECT_FALSE(config.server.http2.enableH2cUpgrade);
  EXPECT_EQ(config.server.http2.maxConcurrentStreams, 256U);
  EXPECT_EQ(config.server.http2.initialWindowSize, 131070U);
  EXPECT_EQ(config.server.http2.maxFrameSize, 32768U);
  EXPECT_EQ(config.server.http2.maxHeaderListSize, 16384U);
  EXPECT_EQ(config.server.http2.connectionWindowSize, 2097152U);
  EXPECT_EQ(config.server.http2.settingsTimeout, std::chrono::milliseconds{3000});
  EXPECT_EQ(config.server.http2.pingInterval, std::chrono::milliseconds{30000});
  EXPECT_EQ(config.server.http2.pingTimeout, std::chrono::milliseconds{5000});
  EXPECT_EQ(config.server.http2.maxStreamsPerConnection, 1000U);
  EXPECT_EQ(config.server.http2.maxPriorityTreeDepth, 128U);
  EXPECT_EQ(config.server.http2.headerTableSize, 8192U);
  EXPECT_FALSE(config.server.http2.enablePriority);
  EXPECT_FALSE(config.server.http2.mergeUnknownRequestHeaders);
}
#endif

// ============================================================================
// Compression configuration
// ============================================================================
TEST(ConfigLoaderTest, CompressionConfig) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "compression": {
        "preferredFormats": ["br", "zstd", "gzip"],
        "addVaryAcceptEncodingHeader": false,
        "defaultDirectCompressionMode": "on",
        "maxCompressRatio": 0.8,
        "initialCompressionBufferLimit": 65536,
        "minBytes": 2048,
        "contentTypeAllowList": ["text/html", "application/json"],
        "zlib": {"level": 6},
        "zstd": {"compressionLevel": 3, "windowLog": 22},
        "brotli": {"quality": 4, "window": 20}
      }
    }
  })",
                                          ConfigFormat::json);
  ASSERT_EQ(config.server.compression.preferredFormats.size(), 3U);
  EXPECT_EQ(config.server.compression.preferredFormats[0], Encoding::br);
  EXPECT_EQ(config.server.compression.preferredFormats[1], Encoding::zstd);
  EXPECT_EQ(config.server.compression.preferredFormats[2], Encoding::gzip);
  EXPECT_FALSE(config.server.compression.addVaryAcceptEncodingHeader);
  EXPECT_EQ(config.server.compression.defaultDirectCompressionMode, DirectCompressionMode::On);
  EXPECT_FLOAT_EQ(config.server.compression.maxCompressRatio, 0.8F);
  EXPECT_EQ(config.server.compression.initialCompressionBufferLimit, 65536U);
  EXPECT_EQ(config.server.compression.minBytes, 2048U);
  EXPECT_EQ(config.server.compression.zlib.level, 6);
  EXPECT_EQ(config.server.compression.zstd.compressionLevel, 3);
  EXPECT_EQ(config.server.compression.zstd.windowLog, 22);
  EXPECT_EQ(config.server.compression.brotli.quality, 4);
  EXPECT_EQ(config.server.compression.brotli.window, 20);

  std::vector<std::string_view> allowList;
  for (auto sv : config.server.compression.contentTypeAllowList) {
    allowList.push_back(sv);
  }
  ASSERT_EQ(allowList.size(), 2U);
  EXPECT_EQ(allowList[0], "text/html");
  EXPECT_EQ(allowList[1], "application/json");
}

// ============================================================================
// Decompression configuration
// ============================================================================
TEST(ConfigLoaderTest, DecompressionConfig) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "decompression": {
        "enable": false,
        "maxCompressedBytes": 1048576,
        "maxDecompressedBytes": 10485760,
        "decoderChunkSize": 65536,
        "streamingDecompressionThresholdBytes": 8388608,
        "maxExpansionRatio": 100.0
      }
    }
  })",
                                          ConfigFormat::json);
  EXPECT_FALSE(config.server.decompression.enable);
  EXPECT_EQ(config.server.decompression.maxCompressedBytes, 1048576U);
  EXPECT_EQ(config.server.decompression.maxDecompressedBytes, 10485760U);
  EXPECT_EQ(config.server.decompression.decoderChunkSize, 65536U);
  EXPECT_EQ(config.server.decompression.streamingDecompressionThresholdBytes, 8388608U);
  EXPECT_DOUBLE_EQ(config.server.decompression.maxExpansionRatio, 100.0);
}

// ============================================================================
// Telemetry configuration
// ============================================================================
TEST(ConfigLoaderTest, TelemetryConfig) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "telemetry": {
        "otelEnabled": true,
        "dogStatsDEnabled": true,
        "sampleRate": 0.5,
        "exportInterval": "10s",
        "exportTimeout": "5s",
        "endpoint": "http://localhost:4318",
        "serviceName": "my-service",
        "dogstatsdSocketPath": "/var/run/datadog/dsd.socket",
        "dogstatsdNamespace": "myns",
        "dogstatsdTags": ["env:prod", "region:us-east"],
        "httpHeaders": ["Authorization:Bearer token123"]
      }
    }
  })",
                                          ConfigFormat::json);
  EXPECT_TRUE(config.server.telemetry.otelEnabled);
  EXPECT_TRUE(config.server.telemetry.dogStatsDEnabled);
  EXPECT_FLOAT_EQ(config.server.telemetry.sampleRate, 0.5F);
  EXPECT_EQ(config.server.telemetry.exportInterval, std::chrono::milliseconds{10000});
  EXPECT_EQ(config.server.telemetry.exportTimeout, std::chrono::milliseconds{5000});
  EXPECT_EQ(config.server.telemetry.endpoint(), "http://localhost:4318");
  EXPECT_EQ(config.server.telemetry.serviceName(), "my-service");
  EXPECT_EQ(config.server.telemetry.dogstatsdSocketPath(), "/var/run/datadog/dsd.socket");
  EXPECT_EQ(config.server.telemetry.dogstatsdNamespace(), "myns");

  std::vector<std::string_view> tags;
  for (auto sv : config.server.telemetry.dogstatsdTags()) {
    tags.push_back(sv);
  }
  // validate() auto-appends "service:my-service" tag from serviceName
  ASSERT_EQ(tags.size(), 3U);
  EXPECT_EQ(tags[0], "env:prod");
  EXPECT_EQ(tags[1], "region:us-east");
  EXPECT_EQ(tags[2], "service:my-service");
}

// ============================================================================
// Builtin probes configuration
// ============================================================================
TEST(ConfigLoaderTest, BuiltinProbesConfig) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "builtinProbes": {
        "enabled": true,
        "livenessPath": "/healthz",
        "readinessPath": "/readyz",
        "startupPath": "/startupz"
      }
    }
  })",
                                          ConfigFormat::json);
  EXPECT_TRUE(config.server.builtinProbes.enabled);
  EXPECT_EQ(config.server.builtinProbes.livenessPath(), "/healthz");
  EXPECT_EQ(config.server.builtinProbes.readinessPath(), "/readyz");
  EXPECT_EQ(config.server.builtinProbes.startupPath(), "/startupz");
}

// ============================================================================
// Global headers
// ============================================================================
TEST(ConfigLoaderTest, GlobalHeaders) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "globalHeaders": ["server: myserver", "x-custom: value"]
    }
  })",
                                          ConfigFormat::json);
  std::vector<std::string_view> headers;
  for (auto sv : config.server.globalHeaders) {
    headers.push_back(sv);
  }
  ASSERT_EQ(headers.size(), 2U);
  EXPECT_EQ(headers[0], "server: myserver");
  EXPECT_EQ(headers[1], "x-custom: value");
}

// ============================================================================
// Connect allowlist
// ============================================================================
TEST(ConfigLoaderTest, ConnectAllowlist) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "connectAllowlist": ["example.com", "10.0.0.1"]
    }
  })",
                                          ConfigFormat::json);
  std::vector<std::string_view> allowlist;
  for (auto sv : config.server.connectAllowlist()) {
    allowlist.push_back(sv);
  }
  ASSERT_EQ(allowlist.size(), 2U);
  EXPECT_EQ(allowlist[0], "example.com");
  EXPECT_EQ(allowlist[1], "10.0.0.1");
}

// ============================================================================
// Router configuration
// ============================================================================
TEST(ConfigLoaderTest, RouterTrailingSlashPolicy) {
  auto config = detail::ParseConfigString(R"({"router":{"trailingSlashPolicy":"strict"}})", ConfigFormat::json);
  EXPECT_EQ(config.router.trailingSlashPolicy, RouterConfig::TrailingSlashPolicy::Strict);
}

TEST(ConfigLoaderTest, RouterTrailingSlashPolicyRedirect) {
  auto config = detail::ParseConfigString(R"({"router":{"trailingSlashPolicy":"redirect"}})", ConfigFormat::json);
  EXPECT_EQ(config.router.trailingSlashPolicy, RouterConfig::TrailingSlashPolicy::Redirect);
}

TEST(ConfigLoaderTest, RouterCorsPolicy) {
  auto config = detail::ParseConfigString(R"({
    "router": {
      "defaultCorsPolicy": {
        "active": true,
        "allowedOrigins": ["https://example.com"],
        "allowedMethods": ["GET", "POST"],
        "allowedRequestHeaders": ["Content-Type"],
        "exposedHeaders": ["X-Request-Id"],
        "allowCredentials": true,
        "allowPrivateNetwork": false,
        "maxAgeSeconds": 3600
      }
    }
  })",
                                          ConfigFormat::json);
  EXPECT_TRUE(config.router.defaultCorsPolicy.active());
}

TEST(ConfigLoaderTest, RouterCorsPolicyWildcard) {
  auto config = detail::ParseConfigString(R"({
    "router": {
      "defaultCorsPolicy": {
        "active": true,
        "allowedOrigins": ["*"],
        "allowedRequestHeaders": ["*"]
      }
    }
  })",
                                          ConfigFormat::json);
  EXPECT_TRUE(config.router.defaultCorsPolicy.active());
}

TEST(ConfigLoaderTest, RouterCorsPolicyInactive) {
  auto config = detail::ParseConfigString(R"({
    "router": {
      "defaultCorsPolicy": {
        "active": false
      }
    }
  })",
                                          ConfigFormat::json);
  EXPECT_FALSE(config.router.defaultCorsPolicy.active());
}

TEST(ConfigLoaderTest, RouterCorsPolicyRoundTrip) {
  TopLevelConfig original;
  auto& cors = original.router.defaultCorsPolicy;
  cors.allowOrigin("https://one.example").allowOrigin("https://two.example");
  cors.allowCredentials(true);
  cors.allowPrivateNetwork(true);
  cors.allowMethods(static_cast<http::MethodBmp>(http::Method::GET) | static_cast<http::MethodBmp>(http::Method::POST) |
                    static_cast<http::MethodBmp>(http::Method::PUT));
  cors.allowRequestHeader("Content-Type").allowRequestHeader("Authorization");
  cors.exposeHeader("X-Request-Id").exposeHeader("X-Trace-Id");
  cors.maxAge(std::chrono::seconds{7200});

  auto json = detail::SerializeConfig(original, ConfigFormat::json);
  EXPECT_FALSE(json.empty());

  auto loaded = detail::ParseConfigString(json, ConfigFormat::json);
  EXPECT_TRUE(loaded.router.defaultCorsPolicy.active());

  // Re-serialize and compare - verifies full round-trip fidelity
  auto json2 = detail::SerializeConfig(loaded, ConfigFormat::json);
  EXPECT_EQ(json, json2);
}

TEST(ConfigLoaderTest, RouterCorsPolicyWildcardRoundTrip) {
  TopLevelConfig original;
  original.router.defaultCorsPolicy.allowAnyOrigin().allowAnyRequestHeaders().maxAge(std::chrono::seconds{600});

  auto json = detail::SerializeConfig(original, ConfigFormat::json);
  auto loaded = detail::ParseConfigString(json, ConfigFormat::json);
  EXPECT_TRUE(loaded.router.defaultCorsPolicy.active());

  auto json2 = detail::SerializeConfig(loaded, ConfigFormat::json);
  EXPECT_EQ(json, json2);
}

TEST(ConfigLoaderTest, RouterCorsPolicyYamlRoundTrip) {
  TopLevelConfig original;
  original.router.defaultCorsPolicy.allowOrigin("https://yaml.example")
      .allowMethods(static_cast<http::MethodBmp>(http::Method::GET))
      .exposeHeader("X-Custom")
      .maxAge(std::chrono::seconds{300});

  auto yaml = detail::SerializeConfig(original, ConfigFormat::yaml);
  EXPECT_FALSE(yaml.empty());

  auto loaded = detail::ParseConfigString(yaml, ConfigFormat::yaml);
  EXPECT_TRUE(loaded.router.defaultCorsPolicy.active());

  // Verify CORS fidelity: serialize both routers to JSON and compare
  auto routerJsonOriginal = glz::write_json(original.router).value_or(std::string{});
  auto routerJsonLoaded = glz::write_json(loaded.router).value_or(std::string{});
  EXPECT_EQ(routerJsonOriginal, routerJsonLoaded);
}

TEST(ConfigLoaderTest, RouterCorsPolicyEmptyAndDuplicateOrigins) {
  auto config = detail::ParseConfigString(R"({
    "router": {
      "defaultCorsPolicy": {
        "active": true,
        "allowedOrigins": ["https://example.com", "", "https://example.com"],
        "allowedRequestHeaders": ["Content-Type", ""],
        "exposedHeaders": ["X-Request-Id", ""]
      }
    }
  })",
                                          ConfigFormat::json);
  EXPECT_TRUE(config.router.defaultCorsPolicy.active());
}

TEST(ConfigLoaderTest, RouterCorsPolicyUnknownMethod) {
  auto config = detail::ParseConfigString(R"({
    "router": {
      "defaultCorsPolicy": {
        "active": true,
        "allowedOrigins": ["*"],
        "allowedMethods": ["GET", "INVALID_METHOD"]
      }
    }
  })",
                                          ConfigFormat::json);
  EXPECT_TRUE(config.router.defaultCorsPolicy.active());
}

// ============================================================================
// JSON round-trip
// ============================================================================
TEST(ConfigLoaderTest, JsonRoundTrip) {
  TopLevelConfig original;
  original.server.port = 9090;
  original.server.nbThreads = 8;
  original.server.keepAliveTimeout = std::chrono::milliseconds{10000};
  original.server.tls.enabled = true;
  original.server.tls.withCertFile("/test/cert.pem");
  original.server.tls.withKeyFile("/test/key.pem");
  original.server.builtinProbes.enabled = true;
  original.server.builtinProbes.withLivenessPath("/health");
  original.router.trailingSlashPolicy = RouterConfig::TrailingSlashPolicy::Strict;

  auto json = detail::SerializeConfig(original, ConfigFormat::json);
  EXPECT_FALSE(json.empty());

  auto loaded = detail::ParseConfigString(json, ConfigFormat::json);
  EXPECT_EQ(loaded.server.port, 9090U);
  EXPECT_EQ(loaded.server.nbThreads, 8U);
  EXPECT_EQ(loaded.server.keepAliveTimeout, std::chrono::milliseconds{10000});
  EXPECT_TRUE(loaded.server.tls.enabled);
  EXPECT_EQ(loaded.server.tls.certFile(), "/test/cert.pem");
  EXPECT_EQ(loaded.server.tls.keyFile(), "/test/key.pem");
  EXPECT_TRUE(loaded.server.builtinProbes.enabled);
  EXPECT_EQ(loaded.server.builtinProbes.livenessPath(), "/health");
  EXPECT_EQ(loaded.router.trailingSlashPolicy, RouterConfig::TrailingSlashPolicy::Strict);
}

// ============================================================================
// YAML format
// ============================================================================
TEST(ConfigLoaderTest, YamlBasic) {
  auto config = detail::ParseConfigString(
      "server:\n"
      "  port: 3000\n"
      "  nbThreads: 2\n"
      "  keepAliveTimeout: 5s\n"
      "  tls:\n"
      "    enabled: true\n"
      "    certFile: /etc/tls/cert.pem\n"
      "    keyFile: /etc/tls/key.pem\n"
      "router:\n"
      "  trailingSlashPolicy: normalize\n",
      ConfigFormat::yaml);
  EXPECT_EQ(config.server.port, 3000U);
  EXPECT_EQ(config.server.nbThreads, 2U);
  EXPECT_EQ(config.server.keepAliveTimeout, std::chrono::milliseconds{5000});
  EXPECT_TRUE(config.server.tls.enabled);
  EXPECT_EQ(config.server.tls.certFile(), "/etc/tls/cert.pem");
  EXPECT_EQ(config.server.tls.keyFile(), "/etc/tls/key.pem");
  EXPECT_EQ(config.router.trailingSlashPolicy, RouterConfig::TrailingSlashPolicy::Normalize);
}

TEST(ConfigLoaderTest, YamlRoundTrip) {
  TopLevelConfig original;
  original.server.port = 4433;
  original.server.tls.enabled = true;
  original.server.tls.withCertFile("/tls/cert");
  original.server.tls.withKeyFile("/tls/key");

  auto yaml = detail::SerializeConfig(original, ConfigFormat::yaml);
  EXPECT_FALSE(yaml.empty());

  auto loaded = detail::ParseConfigString(yaml, ConfigFormat::yaml);
  EXPECT_EQ(loaded.server.port, 4433U);
  EXPECT_TRUE(loaded.server.tls.enabled);
  EXPECT_EQ(loaded.server.tls.certFile(), "/tls/cert");
  EXPECT_EQ(loaded.server.tls.keyFile(), "/tls/key");
}

// ============================================================================
// File loading
// ============================================================================
TEST(ConfigLoaderTest, LoadFromJsonFile) {
  auto tmpDir = std::filesystem::temp_directory_path();
  auto filePath = tmpDir / "aeronet_test_config.json";
  {
    std::ofstream ofs(filePath);
    ofs << R"({"server":{"port":7777},"router":{"trailingSlashPolicy":"strict"}})";
  }
  auto config = detail::ParseConfigFile(filePath);
  EXPECT_EQ(config.server.port, 7777U);
  EXPECT_EQ(config.router.trailingSlashPolicy, RouterConfig::TrailingSlashPolicy::Strict);
  std::filesystem::remove(filePath);
}

TEST(ConfigLoaderTest, LoadFromYamlFile) {
  auto tmpDir = std::filesystem::temp_directory_path();
  auto filePath = tmpDir / "aeronet_test_config.yaml";
  {
    std::ofstream ofs(filePath);
    ofs << "server:\n  port: 8888\n";
  }
  auto config = detail::ParseConfigFile(filePath);
  EXPECT_EQ(config.server.port, 8888U);
  std::filesystem::remove(filePath);
}

TEST(ConfigLoaderTest, LoadFromYmlFile) {
  auto tmpDir = std::filesystem::temp_directory_path();
  auto filePath = tmpDir / "aeronet_test_config.yml";
  {
    std::ofstream ofs(filePath);
    ofs << "server:\n  port: 9999\n";
  }
  auto config = detail::ParseConfigFile(filePath);
  EXPECT_EQ(config.server.port, 9999U);
  std::filesystem::remove(filePath);
}

TEST(ConfigLoaderTest, LoadServerConfig) {
  auto tmpDir = std::filesystem::temp_directory_path();
  auto filePath = tmpDir / "aeronet_test_server.json";
  {
    std::ofstream ofs(filePath);
    ofs << R"({"server":{"port":6666}})";
  }
  auto serverConfig = LoadServerConfig(filePath);
  EXPECT_EQ(serverConfig.port, 6666U);
  std::filesystem::remove(filePath);
}

// ============================================================================
// Error handling
// ============================================================================
TEST(ConfigLoaderTest, InvalidJson) {
  EXPECT_THROW(detail::ParseConfigString("{not valid json}", ConfigFormat::json), std::runtime_error);
}

TEST(ConfigLoaderTest, MissingFile) {
  EXPECT_THROW(detail::ParseConfigFile(std::filesystem::path("/nonexistent/path/config.json")), std::runtime_error);
}

TEST(ConfigLoaderTest, UnknownExtension) {
  auto tmpDir = std::filesystem::temp_directory_path();
  auto filePath = tmpDir / "aeronet_test_config.xml";
  {
    std::ofstream ofs(filePath);
    ofs << "{}";
  }
  EXPECT_THROW(detail::ParseConfigFile(filePath), std::runtime_error);
  std::filesystem::remove(filePath);
}

// ============================================================================
// HttpResponse::bodyJson / bodyYaml
// ============================================================================

struct SampleDto {
  int id{};
  std::string name;
  bool active{false};
};

TEST(HttpResponseBodySerializationTest, BodyJsonBasic) {
  SampleDto dto{.id = 42, .name = "test", .active = true};
  HttpResponse resp;
  resp.bodyJson(dto);

  auto body = resp.bodyInMemory();
  EXPECT_TRUE(body.contains("\"id\":42"));
  EXPECT_TRUE(body.contains("\"name\":\"test\""));
  EXPECT_TRUE(body.contains("\"active\":true"));
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeApplicationJson);
}

TEST(HttpResponseBodySerializationTest, BodyJsonEmptyObject) {
  SampleDto dto{};
  HttpResponse resp;
  resp.bodyJson(dto);

  auto body = resp.bodyInMemory();
  EXPECT_TRUE(body.contains("\"id\":0"));
  EXPECT_TRUE(body.contains("\"name\":\"\""));
  EXPECT_TRUE(body.contains("\"active\":false"));
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeApplicationJson);
}

TEST(HttpResponseBodySerializationTest, BodyJsonChaining) {
  SampleDto dto{.id = 1, .name = "chained", .active = false};
  auto resp = HttpResponse(http::StatusCodeOK).bodyJson(dto);

  EXPECT_TRUE(resp.bodyInMemory().contains("\"id\":1"));
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
}

TEST(HttpResponseBodySerializationTest, BodyJsonRvalueOverload) {
  SampleDto dto{.id = 7, .name = "rvalue"};
  HttpResponse resp = std::move(HttpResponse()).bodyJson(dto);

  EXPECT_TRUE(resp.bodyInMemory().contains("\"id\":7"));
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeApplicationJson);
}

TEST(HttpResponseBodySerializationTest, BodyJsonOverwritesPreviousBody) {
  HttpResponse resp;
  resp.body("old body");
  SampleDto dto{.id = 99, .name = "new"};
  resp.bodyJson(dto);

  EXPECT_TRUE(resp.bodyInMemory().contains("\"id\":99"));
  EXPECT_FALSE(resp.bodyInMemory().contains("old body"));
}

TEST(HttpResponseBodySerializationTest, BodyJsonWithSpecialChars) {
  SampleDto dto{.id = 1, .name = "hello \"world\" \t\n"};
  HttpResponse resp;
  resp.bodyJson(dto);

  auto body = resp.bodyInMemory();
  // JSON escape sequences should be properly handled
  EXPECT_TRUE(body.contains("\\\"world\\\""));
  EXPECT_TRUE(body.contains("\\t"));
  EXPECT_TRUE(body.contains("\\n"));
}

TEST(HttpResponseBodySerializationTest, BodyYamlBasic) {
  SampleDto dto{.id = 10, .name = "yaml-test", .active = true};
  HttpResponse resp;
  resp.bodyYaml(dto);

  auto body = resp.bodyInMemory();
  EXPECT_TRUE(body.contains("id: 10"));
  EXPECT_TRUE(body.contains("name: yaml-test"));
  EXPECT_TRUE(body.contains("active: true"));
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/yaml");
}

TEST(HttpResponseBodySerializationTest, BodyYamlEmptyObject) {
  SampleDto dto{};
  HttpResponse resp;
  resp.bodyYaml(dto);

  auto body = resp.bodyInMemory();
  EXPECT_TRUE(body.contains("id: 0"));
  EXPECT_TRUE(body.contains("name:"));
  EXPECT_TRUE(body.contains("active: false"));
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/yaml");
}

TEST(HttpResponseBodySerializationTest, BodyYamlChaining) {
  SampleDto dto{.id = 3, .name = "chained-yaml", .active = true};
  auto resp = HttpResponse(http::StatusCodeOK).bodyYaml(dto);

  EXPECT_TRUE(resp.bodyInMemory().contains("id: 3"));
  EXPECT_EQ(resp.status(), http::StatusCodeOK);
}

TEST(HttpResponseBodySerializationTest, BodyYamlRvalueOverload) {
  SampleDto dto{.id = 5, .name = "rvalue-yaml"};
  HttpResponse resp = std::move(HttpResponse()).bodyYaml(dto);

  EXPECT_TRUE(resp.bodyInMemory().contains("id: 5"));
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/yaml");
}

TEST(HttpResponseBodySerializationTest, BodyYamlOverwritesPreviousBody) {
  HttpResponse resp;
  resp.body("some previous content");
  SampleDto dto{.id = 77, .name = "overwrite"};
  resp.bodyYaml(dto);

  EXPECT_TRUE(resp.bodyInMemory().contains("id: 77"));
  EXPECT_FALSE(resp.bodyInMemory().contains("previous"));
}

TEST(HttpResponseBodySerializationTest, BodyJsonThenYaml) {
  SampleDto dto{.id = 1, .name = "switch"};
  HttpResponse resp;
  resp.bodyJson(dto);
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), http::ContentTypeApplicationJson);

  resp.bodyYaml(dto);
  EXPECT_EQ(resp.headerValueOrEmpty(http::ContentType), "text/yaml");
  EXPECT_TRUE(resp.bodyInMemory().contains("id: 1"));
}

TEST(HttpResponseBodySerializationTest, BodyJsonMap) {
  std::map<std::string, int> mp{{"alpha", 1}, {"beta", 2}};
  HttpResponse resp;
  resp.bodyJson(mp);

  auto body = resp.bodyInMemory();
  EXPECT_TRUE(body.contains("\"alpha\":1"));
  EXPECT_TRUE(body.contains("\"beta\":2"));
}

TEST(HttpResponseBodySerializationTest, BodyJsonVector) {
  std::vector<int> vec{10, 20, 30};
  HttpResponse resp;
  resp.bodyJson(vec);

  EXPECT_EQ(resp.bodyInMemory(), "[10,20,30]");
}

// A type whose custom Glaze serializer always fails - used to exercise the
// error path in bodyJson / bodyYaml.
struct BadSerializerDto {
  int val{};
};

}  // namespace aeronet

template <>
struct glz::to<glz::JSON, aeronet::BadSerializerDto> {
  template <auto Opts, class... Args>
  static void op(auto&&, glz::is_context auto&& ctx, Args&&...) {
    ctx.error = glz::error_code::syntax_error;
  }
};

template <>
struct glz::to<glz::YAML, aeronet::BadSerializerDto> {
  template <auto Opts, class... Args>
  static void op(auto&&, glz::is_context auto&& ctx, Args&&...) {
    ctx.error = glz::error_code::syntax_error;
  }
};

namespace aeronet {

TEST(HttpResponseBodySerializationTest, BodyJsonBadSerializerThrows) {
  BadSerializerDto bad{.val = 1};
  HttpResponse resp;
  EXPECT_THROW(resp.bodyJson(bad), std::runtime_error);
}

TEST(HttpResponseBodySerializationTest, BodyYamlBadSerializerThrows) {
  BadSerializerDto bad{.val = 2};
  HttpResponse resp;
  EXPECT_THROW(resp.bodyYaml(bad), std::runtime_error);
}

// ============================================================================
// Full Kubernetes-style config
// ============================================================================
TEST(ConfigLoaderTest, KubernetesStyleYaml) {
  auto config = detail::ParseConfigString(
      "server:\n"
      "  port: 8080\n"
      "  nbThreads: 0\n"
      "  tls:\n"
      "    enabled: true\n"
      "    certFile: /etc/tls/tls.crt\n"
      "    keyFile: /etc/tls/tls.key\n"
      "    cipherPolicy: modern\n"
      "  builtinProbes:\n"
      "    enabled: true\n"
      "    livenessPath: /healthz\n"
      "    readinessPath: /readyz\n"
      "    startupPath: /startupz\n"
      "  compression:\n"
      "    preferredFormats:\n"
      "      - br\n"
      "      - zstd\n"
      "      - gzip\n"
      "    minBytes: 1024\n",
      ConfigFormat::yaml);
  EXPECT_EQ(config.server.port, 8080U);
  EXPECT_TRUE(config.server.tls.enabled);
  EXPECT_EQ(config.server.tls.certFile(), "/etc/tls/tls.crt");
  EXPECT_EQ(config.server.tls.cipherPolicy, TLSConfig::CipherPolicy::Modern);
  EXPECT_TRUE(config.server.builtinProbes.enabled);
  EXPECT_EQ(config.server.builtinProbes.livenessPath(), "/healthz");
  ASSERT_EQ(config.server.compression.preferredFormats.size(), 3U);
  EXPECT_EQ(config.server.compression.preferredFormats[0], Encoding::br);
}

// ============================================================================
// LoadServerConfig from string buffer
// ============================================================================
TEST(ConfigLoaderTest, LoadServerConfigFromString) {
  auto serverConfig = LoadServerConfig(R"({"server":{"port":5555}})", ConfigFormat::json);
  EXPECT_EQ(serverConfig.port, 5555U);
}

TEST(ConfigLoaderTest, LoadServerConfigFromYamlString) {
  auto serverConfig = LoadServerConfig("server:\n  port: 4444\n", ConfigFormat::yaml);
  EXPECT_EQ(serverConfig.port, 4444U);
}

// ============================================================================
// Server filepath constructors
// ============================================================================
TEST(ServerConfigConstructorTest, SingleHttpServerFromJsonFile) {
  auto tmpDir = std::filesystem::temp_directory_path();
  auto filePath = tmpDir / "aeronet_single_test.json";
  {
    std::ofstream ofs(filePath);
    ofs << R"({"server":{"port":0},"router":{"trailingSlashPolicy":"strict"}})";
  }
  SingleHttpServer server(filePath);
  EXPECT_NE(server.port(), 0U);
  std::filesystem::remove(filePath);
}

TEST(ServerConfigConstructorTest, SingleHttpServerFromYamlFile) {
  auto tmpDir = std::filesystem::temp_directory_path();
  auto filePath = tmpDir / "aeronet_single_test.yaml";
  {
    std::ofstream ofs(filePath);
    ofs << "server:\n  port: 0\n";
  }
  SingleHttpServer server(filePath);
  EXPECT_NE(server.port(), 0U);
  std::filesystem::remove(filePath);
}

TEST(ServerConfigConstructorTest, MultiHttpServerFromJsonFile) {
  auto tmpDir = std::filesystem::temp_directory_path();
  auto filePath = tmpDir / "aeronet_multi_test.json";
  {
    std::ofstream ofs(filePath);
    ofs << R"({"server":{"port":0,"nbThreads":2}})";
  }
  MultiHttpServer server(filePath);
  EXPECT_NE(server.port(), 0U);
  EXPECT_EQ(server.nbThreads(), 2U);
  std::filesystem::remove(filePath);
}

TEST(ServerConfigConstructorTest, MultiHttpServerFromYamlFile) {
  auto tmpDir = std::filesystem::temp_directory_path();
  auto filePath = tmpDir / "aeronet_multi_test.yaml";
  {
    std::ofstream ofs(filePath);
    ofs << "server:\n  port: 0\n  nbThreads: 2\n";
  }
  MultiHttpServer server(filePath);
  EXPECT_NE(server.port(), 0U);
  EXPECT_EQ(server.nbThreads(), 2U);
  std::filesystem::remove(filePath);
}

TEST(ServerConfigConstructorTest, SingleHttpServerMissingFile) {
  EXPECT_THROW(SingleHttpServer("/nonexistent/path/config.json"), std::runtime_error);
}

TEST(ServerConfigConstructorTest, MultiHttpServerMissingFile) {
  EXPECT_THROW(MultiHttpServer(std::filesystem::path("/nonexistent/path/config.json")), std::runtime_error);
}

// ============================================================================
// dumpConfig / saveConfig
// ============================================================================
TEST(ServerConfigDumpTest, SingleHttpServerDumpConfigJson) {
  auto tmpDir = std::filesystem::temp_directory_path();
  auto filePath = tmpDir / "aeronet_dump_single.json";
  {
    std::ofstream ofs(filePath);
    ofs << R"({"server":{"port":0},"router":{"trailingSlashPolicy":"strict"}})";
  }
  SingleHttpServer server(filePath);
  std::filesystem::remove(filePath);

  auto json = server.dumpConfig(ConfigFormat::json);
  EXPECT_FALSE(json.empty());

  auto loaded = detail::ParseConfigString(json, ConfigFormat::json);
  EXPECT_EQ(loaded.router.trailingSlashPolicy, RouterConfig::TrailingSlashPolicy::Strict);
}

TEST(ServerConfigDumpTest, SingleHttpServerDumpConfigYaml) {
  auto tmpDir = std::filesystem::temp_directory_path();
  auto filePath = tmpDir / "aeronet_dump_single.yaml";
  {
    std::ofstream ofs(filePath);
    ofs << "server:\n  port: 0\n";
  }
  SingleHttpServer server(filePath);
  std::filesystem::remove(filePath);

  auto yaml = server.dumpConfig(ConfigFormat::yaml);
  EXPECT_FALSE(yaml.empty());
  EXPECT_TRUE(yaml.contains("port:"));
}

TEST(ServerConfigDumpTest, SingleHttpServerSaveConfigJson) {
  auto tmpDir = std::filesystem::temp_directory_path();

  auto srcPath = tmpDir / "aeronet_save_src.json";
  {
    std::ofstream ofs(srcPath);
    ofs << R"({"server":{"port":0},"router":{"trailingSlashPolicy":"strict"}})";
  }
  SingleHttpServer server(srcPath);
  std::filesystem::remove(srcPath);

  auto dstPath = tmpDir / "aeronet_save_dst.json";
  server.saveConfig(dstPath);

  auto loaded = detail::ParseConfigFile(dstPath);
  EXPECT_EQ(loaded.router.trailingSlashPolicy, RouterConfig::TrailingSlashPolicy::Strict);
  std::filesystem::remove(dstPath);
}

TEST(ServerConfigDumpTest, SingleHttpServerSaveConfigYaml) {
  auto tmpDir = std::filesystem::temp_directory_path();

  auto srcPath = tmpDir / "aeronet_save_src.yaml";
  {
    std::ofstream ofs(srcPath);
    ofs << "server:\n  port: 0\n";
  }
  SingleHttpServer server(srcPath);
  std::filesystem::remove(srcPath);

  auto dstPath = tmpDir / "aeronet_save_dst.yaml";
  server.saveConfig(dstPath);

  auto loaded = detail::ParseConfigFile(dstPath);
  EXPECT_NE(loaded.server.port, 0U);
  std::filesystem::remove(dstPath);
}

TEST(ServerConfigDumpTest, MultiHttpServerDumpConfigJson) {
  auto tmpDir = std::filesystem::temp_directory_path();
  auto filePath = tmpDir / "aeronet_dump_multi.json";
  {
    std::ofstream ofs(filePath);
    ofs << R"({"server":{"port":0,"nbThreads":2},"router":{"trailingSlashPolicy":"strict"}})";
  }
  MultiHttpServer server(filePath);
  std::filesystem::remove(filePath);

  auto json = server.dumpConfig(ConfigFormat::json);
  EXPECT_FALSE(json.empty());

  auto loaded = detail::ParseConfigString(json, ConfigFormat::json);
  EXPECT_EQ(loaded.server.nbThreads, 2U);
  EXPECT_EQ(loaded.router.trailingSlashPolicy, RouterConfig::TrailingSlashPolicy::Strict);
}

TEST(ServerConfigDumpTest, MultiHttpServerSaveConfigYaml) {
  auto tmpDir = std::filesystem::temp_directory_path();

  auto srcPath = tmpDir / "aeronet_save_multi.json";
  {
    std::ofstream ofs(srcPath);
    ofs << R"({"server":{"port":0,"nbThreads":2}})";
  }
  MultiHttpServer server(srcPath);
  std::filesystem::remove(srcPath);

  auto dstPath = tmpDir / "aeronet_save_multi.yaml";
  server.saveConfig(dstPath);

  auto loaded = detail::ParseConfigFile(dstPath);
  EXPECT_EQ(loaded.server.nbThreads, 2U);
  std::filesystem::remove(dstPath);
}

TEST(ServerConfigDumpTest, MultiHttpServerSaveConfigJson) {
  auto tmpDir = std::filesystem::temp_directory_path();

  auto srcPath = tmpDir / "aeronet_save_multi_src.yaml";
  {
    std::ofstream ofs(srcPath);
    ofs << "server:\n  port: 0\n  nbThreads: 2\n";
  }
  MultiHttpServer server(srcPath);
  std::filesystem::remove(srcPath);

  auto dstPath = tmpDir / "aeronet_save_multi_dst.json";
  server.saveConfig(dstPath);

  auto loaded = detail::ParseConfigFile(dstPath);
  EXPECT_EQ(loaded.server.nbThreads, 2U);
  std::filesystem::remove(dstPath);
}

TEST(ServerConfigDumpTest, MultiHttpServerSaveConfigYml) {
  auto tmpDir = std::filesystem::temp_directory_path();

  auto srcPath = tmpDir / "aeronet_save_multi_yml_src.json";
  {
    std::ofstream ofs(srcPath);
    ofs << R"({"server":{"port":0,"nbThreads":2}})";
  }
  MultiHttpServer server(srcPath);
  std::filesystem::remove(srcPath);

  auto dstPath = tmpDir / "aeronet_save_multi_dst.yml";
  server.saveConfig(dstPath);

  auto loaded = detail::ParseConfigFile(dstPath);
  EXPECT_EQ(loaded.server.nbThreads, 2U);
  std::filesystem::remove(dstPath);
}

TEST(ServerConfigDumpTest, MultiHttpServerSaveConfigUnknownExtension) {
  auto tmpDir = std::filesystem::temp_directory_path();

  auto srcPath = tmpDir / "aeronet_save_multi_unkext.json";
  {
    std::ofstream ofs(srcPath);
    ofs << R"({"server":{"port":0,"nbThreads":2}})";
  }
  MultiHttpServer server(srcPath);
  std::filesystem::remove(srcPath);

  EXPECT_THROW(server.saveConfig(tmpDir / "config.txt"), std::runtime_error);
}

// ============================================================================
// Coverage: moved-from MultiHttpServer dumpConfig/saveConfig should throw
// ============================================================================
TEST(ServerConfigDumpTest, MultiHttpServerMovedFromDumpConfigThrows) {
  auto tmpDir = std::filesystem::temp_directory_path();
  auto srcPath = tmpDir / "aeronet_moved_dump.json";
  {
    std::ofstream ofs(srcPath);
    ofs << R"({"server":{"port":0,"nbThreads":2}})";
  }
  MultiHttpServer server(srcPath);
  std::filesystem::remove(srcPath);

  MultiHttpServer moved(std::move(server));
  // server is now empty (moved-from)
  EXPECT_THROW((void)server.dumpConfig(), std::logic_error);  // NOLINT(bugprone-use-after-move)
  EXPECT_THROW(server.saveConfig(tmpDir / "should_not_exist.json"), std::logic_error);
}

// ============================================================================
// Coverage: MultiHttpServer(path, Router) constructor
// ============================================================================
TEST(ServerConfigDumpTest, MultiHttpServerConfigPathAndRouterConstructor) {
  auto tmpDir = std::filesystem::temp_directory_path();
  auto filePath = tmpDir / "aeronet_multi_path_router.json";
  {
    std::ofstream ofs(filePath);
    ofs << R"({"server":{"port":0,"nbThreads":2}})";
  }

  Router router;
  router.setDefault([]([[maybe_unused]] const HttpRequest& req) { return HttpResponse{}.body("from-router"); });

  MultiHttpServer server(filePath, std::move(router));
  std::filesystem::remove(filePath);

  EXPECT_FALSE(server.empty());
  EXPECT_EQ(server.nbThreads(), 2U);
}

TEST(ServerConfigDumpTest, SingleHttpServerRoundTrip) {
  auto tmpDir = std::filesystem::temp_directory_path();

  auto filePath = tmpDir / "aeronet_roundtrip.json";
  {
    std::ofstream ofs(filePath);
    ofs << R"({"server":{"port":0,"enableKeepAlive":false},"router":{"trailingSlashPolicy":"redirect"}})";
  }
  SingleHttpServer server(filePath);
  std::filesystem::remove(filePath);

  // Save config from running server
  auto savedPath = tmpDir / "aeronet_roundtrip_saved.json";
  server.saveConfig(savedPath);

  // Load the saved config and verify fields
  auto loaded = detail::ParseConfigFile(savedPath);
  std::filesystem::remove(savedPath);

  EXPECT_FALSE(loaded.server.enableKeepAlive);
  EXPECT_EQ(loaded.router.trailingSlashPolicy, RouterConfig::TrailingSlashPolicy::Redirect);
  // Port should be the OS-assigned port, not 0
  EXPECT_NE(loaded.server.port, 0U);
}

// ============================================================================
// Coverage: duration parsing edge cases
// ============================================================================
TEST(ConfigLoaderTest, DurationCompoundWithSpaces) {
  auto config = detail::ParseConfigString(R"({"server":{"keepAliveTimeout":"1h 30m"}})", ConfigFormat::json);
  EXPECT_EQ(config.server.keepAliveTimeout, std::chrono::milliseconds{5400000});
}

TEST(ConfigLoaderTest, DurationUnknownUnit) {
  EXPECT_THROW(detail::ParseConfigString(R"({"server":{"keepAliveTimeout":"5x"}})", ConfigFormat::json),
               std::runtime_error);
}

// ============================================================================
// Coverage: chrono::seconds as string from JSON
// ============================================================================
TEST(ConfigLoaderTest, TlsSessionTicketLifetimeAsString) {
  auto config =
      detail::ParseConfigString(R"({"server":{"tls":{"sessionTickets":{"lifetime":"2h"}}}})", ConfigFormat::json);
  EXPECT_EQ(config.server.tls.sessionTickets.lifetime, std::chrono::seconds{7200});
}

// ============================================================================
// Coverage: TelemetryConfig round-trip (exercises getter lambdas)
// ============================================================================
TEST(ConfigLoaderTest, TelemetryConfigRoundTrip) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "telemetry": {
        "dogstatsdTags": ["env:staging", "version:1.2"],
        "httpHeaders": ["X-Api-Key:secret123"],
        "histogramBuckets": {"latency": [1.0, 5.0, 10.0, 50.0]}
      }
    }
  })",
                                          ConfigFormat::json);

  // Round-trip: serialize and re-parse
  auto json = detail::SerializeConfig(config, ConfigFormat::json);
  EXPECT_TRUE(json.find("env:staging") != std::string::npos);
  EXPECT_TRUE(json.find("X-Api-Key:secret123") != std::string::npos);
  EXPECT_TRUE(json.find("latency") != std::string::npos);

  auto reloaded = detail::ParseConfigString(json, ConfigFormat::json);

  std::vector<std::string_view> tags;
  for (auto sv : reloaded.server.telemetry.dogstatsdTags()) {
    tags.push_back(sv);
  }
  ASSERT_EQ(tags.size(), 2U);
  EXPECT_EQ(tags[0], "env:staging");
  EXPECT_EQ(tags[1], "version:1.2");

  std::vector<std::string_view> headers;
  for (auto sv : reloaded.server.telemetry.httpHeadersRange()) {
    headers.push_back(sv);
  }
  ASSERT_EQ(headers.size(), 1U);
  EXPECT_EQ(headers[0], "X-Api-Key:secret123");

  ASSERT_EQ(reloaded.server.telemetry.histogramBuckets().size(), 1U);
}

// ============================================================================
// Coverage: tab whitespace in duration string
// ============================================================================
TEST(ConfigLoaderTest, DurationWithTabWhitespace) {
  auto config = detail::ParseConfigString(R"({"server":{"keepAliveTimeout":"\t2s\t"}})", ConfigFormat::json);
  EXPECT_EQ(config.server.keepAliveTimeout, std::chrono::milliseconds{2000});
}

// ============================================================================
// Coverage: space between number and unit suffix in duration
// ============================================================================
TEST(ConfigLoaderTest, DurationSpaceBetweenNumberAndUnit) {
  auto config = detail::ParseConfigString(R"({"server":{"keepAliveTimeout":"1 h"}})", ConfigFormat::json);
  EXPECT_EQ(config.server.keepAliveTimeout, std::chrono::milliseconds{3600000});
}

// ============================================================================
// Coverage: TLS MajorMinorVersion round-trip (exercises to<Format> non-empty)
// ============================================================================
TEST(ConfigLoaderTest, TlsVersionRoundTrip) {
  auto config = detail::ParseConfigString(R"({
    "server": {
      "tls": {
        "minVersion": "1.2",
        "maxVersion": "1.3"
      }
    }
  })",
                                          ConfigFormat::json);

  ASSERT_TRUE(config.server.tls.minVersion.isValid());
  ASSERT_TRUE(config.server.tls.maxVersion.isValid());

  // Serialize and re-parse to exercise to<Format, MajorMinorVersion> with non-empty value
  auto json = detail::SerializeConfig(config, ConfigFormat::json);
  EXPECT_TRUE(json.find("1.2") != std::string::npos);
  EXPECT_TRUE(json.find("1.3") != std::string::npos);

  auto reloaded = detail::ParseConfigString(json, ConfigFormat::json);
  EXPECT_TRUE(reloaded.server.tls.minVersion.isValid());
  EXPECT_EQ(reloaded.server.tls.minVersion.major(), 1);
  EXPECT_EQ(reloaded.server.tls.minVersion.minor(), 2);
  EXPECT_TRUE(reloaded.server.tls.maxVersion.isValid());
  EXPECT_EQ(reloaded.server.tls.maxVersion.major(), 1);
  EXPECT_EQ(reloaded.server.tls.maxVersion.minor(), 3);
}

// ============================================================================
// Coverage: Duration human-readable round-trip (write as string, read back)
// ============================================================================
TEST(ConfigLoaderTest, DurationHumanReadableRoundTrip) {
  auto config = detail::ParseConfigString(R"({"server":{"keepAliveTimeout":"1h30m"}})", ConfigFormat::json);
  EXPECT_EQ(config.server.keepAliveTimeout, std::chrono::milliseconds{5400000});

  auto json = detail::SerializeConfig(config, ConfigFormat::json);
  // Should contain human-readable duration string, not raw integer
  EXPECT_TRUE(json.contains("1h30m")) << json;

  auto reloaded = detail::ParseConfigString(json, ConfigFormat::json);
  EXPECT_EQ(reloaded.server.keepAliveTimeout, config.server.keepAliveTimeout);
}

TEST(ConfigLoaderTest, DurationSecondsHumanReadableRoundTrip) {
  auto config = detail::ParseConfigString(R"({"server":{"keepAliveTimeout":"3s"}})", ConfigFormat::json);
  EXPECT_EQ(config.server.keepAliveTimeout, std::chrono::milliseconds{3000});

  auto json = detail::SerializeConfig(config, ConfigFormat::json);
  EXPECT_TRUE(json.contains("3s")) << json;

  auto reloaded = detail::ParseConfigString(json, ConfigFormat::json);
  EXPECT_EQ(reloaded.server.keepAliveTimeout, config.server.keepAliveTimeout);
}

TEST(ConfigLoaderTest, DurationZeroRoundTrip) {
  auto config = detail::ParseConfigString(R"({"server":{"keepAliveTimeout":"0ms"}})", ConfigFormat::json);
  EXPECT_EQ(config.server.keepAliveTimeout, std::chrono::milliseconds{0});

  auto json = detail::SerializeConfig(config, ConfigFormat::json);
  EXPECT_TRUE(json.contains("0ms")) << json;

  auto reloaded = detail::ParseConfigString(json, ConfigFormat::json);
  EXPECT_EQ(reloaded.server.keepAliveTimeout, std::chrono::milliseconds{0});
}

TEST(ConfigLoaderTest, DurationSubSecondMillisecondsRoundTrip) {
  auto config = detail::ParseConfigString(R"({"server":{"keepAliveTimeout":"1500ms"}})", ConfigFormat::json);
  EXPECT_EQ(config.server.keepAliveTimeout, std::chrono::milliseconds{1500});

  auto json = detail::SerializeConfig(config, ConfigFormat::json);
  EXPECT_TRUE(json.contains("1s500ms")) << json;

  auto reloaded = detail::ParseConfigString(json, ConfigFormat::json);
  EXPECT_EQ(reloaded.server.keepAliveTimeout, config.server.keepAliveTimeout);
}

// ============================================================================
// Coverage: DetectFormat from detail:: namespace
// ============================================================================
TEST(ConfigLoaderTest, DetectFormatJson) { EXPECT_EQ(detail::DetectFormat("config.json"), ConfigFormat::json); }

TEST(ConfigLoaderTest, DetectFormatYaml) {
  EXPECT_EQ(detail::DetectFormat("config.yaml"), ConfigFormat::yaml);
  EXPECT_EQ(detail::DetectFormat("config.yml"), ConfigFormat::yaml);
}

TEST(ConfigLoaderTest, DetectFormatUnknownThrows) {
  EXPECT_THROW(detail::DetectFormat("config.txt"), std::runtime_error);
}

// ============================================================================
// Coverage: RouterConfig::validate()
// ============================================================================
TEST(ConfigLoaderTest, RouterConfigValidateDefaultIsOk) {
  RouterConfig cfg;
  EXPECT_NO_THROW(cfg.validate());
}

TEST(ConfigLoaderTest, RouterConfigValidateInvalidValues) {
  RouterConfig cfg;

  cfg.trailingSlashPolicy = static_cast<RouterConfig::TrailingSlashPolicy>(999);  // Invalid value
  EXPECT_THROW(cfg.validate(), std::invalid_argument);

  cfg.trailingSlashPolicy = static_cast<RouterConfig::TrailingSlashPolicy>(-1);  // Invalid value
  EXPECT_THROW(cfg.validate(), std::invalid_argument);
}

TEST(ConfigLoaderTest, RouterConfigValidateAllPoliciesOk) {
  RouterConfig cfg;
  cfg.trailingSlashPolicy = RouterConfig::TrailingSlashPolicy::Strict;
  EXPECT_NO_THROW(cfg.validate());
  cfg.trailingSlashPolicy = RouterConfig::TrailingSlashPolicy::Normalize;
  EXPECT_NO_THROW(cfg.validate());
  cfg.trailingSlashPolicy = RouterConfig::TrailingSlashPolicy::Redirect;
  EXPECT_NO_THROW(cfg.validate());
}

// ============================================================================
// Coverage: YAML duration human-readable round-trip
// ============================================================================
TEST(ConfigLoaderTest, DurationHumanReadableYamlRoundTrip) {
  auto config = detail::ParseConfigString(R"({"server":{"keepAliveTimeout":"2m30s"}})", ConfigFormat::json);
  EXPECT_EQ(config.server.keepAliveTimeout, std::chrono::milliseconds{150000});

  auto yaml = detail::SerializeConfig(config, ConfigFormat::yaml);
  EXPECT_TRUE(yaml.contains("2m30s")) << yaml;

  auto reloaded = detail::ParseConfigString(yaml, ConfigFormat::yaml);
  EXPECT_EQ(reloaded.server.keepAliveTimeout, config.server.keepAliveTimeout);
}

TEST(ConfigLoaderTest, TlsVersionYamlAcceptsShortFormDirect) {
  TLSConfig::Version version;

  auto error = glz::read<glz::opts{.format = glz::YAML}>(version, R"("1.3")");
  ASSERT_FALSE(bool(error));

  EXPECT_TRUE(version.isValid());
  EXPECT_EQ(version.major(), 1);
  EXPECT_EQ(version.minor(), 3);
}

TEST(ConfigLoaderTest, TlsVersionYamlAcceptsFullFormDirect) {
  TLSConfig::Version version;

  auto error = glz::read<glz::opts{.format = glz::YAML}>(version, R"("TLS1.2")");
  ASSERT_FALSE(bool(error));

  EXPECT_TRUE(version.isValid());
  EXPECT_EQ(version.major(), 1);
  EXPECT_EQ(version.minor(), 2);
}

TEST(ConfigLoaderTest, TlsVersionYamlRejectsInvalidFullFormDirect) {
  TLSConfig::Version version;

  auto error = glz::read<glz::opts{.format = glz::YAML}>(version, R"("TLS1.10")");

  EXPECT_TRUE(bool(error));
}

TEST(ConfigLoaderTest, TlsVersionYamlSerializesValidVersionDirect) {
  auto yaml = glz::write<glz::opts{.format = glz::YAML}>(TLSConfig::Version{1, 3});
  ASSERT_TRUE(yaml);

  EXPECT_TRUE(yaml.value().contains("1.3"));
}

}  // namespace aeronet
