#include "aeronet/client-config-loader.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <ios>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/client-protocol.hpp"
#include "aeronet/config-file.hpp"
#include "aeronet/glaze-round-trip.hpp"
#include "aeronet/http-client-config-glaze.hpp"  // IWYU pragma: keep (glz::meta of HttpClientConfig)
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-status-code.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"
#include "aeronet/temp-file.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include "aeronet/tls-config.hpp"
#endif

namespace aeronet {

namespace {

using namespace std::chrono_literals;

// JSON of a configuration, to compare two configurations field by field.
std::string ToJson(const HttpClientConfig& config) { return detail::SerializeClientConfig(config, ConfigFormat::json); }

// A valid configuration whose fields all differ from their default value.
HttpClientConfig MakeNonDefaultConfig() {
  HttpClientConfig config;
  config.connectTimeout = 2s;
  config.requestTimeout = 45s;
  config.inactivityTimeout = 1500ms;
  config.keepAliveTimeout = 0ms;
  config.maxResponseBytes = 1UL << 20U;
  config.maxRedirects = 2;
  config.maxIdleConnectionsPerHost = 3;
  config.minReadChunkBytes = 4096;
  config.globalHeaders.clear();
  config.addGlobalHeader(http::Header("user-agent", "my-agent")).addGlobalHeader(http::Header("x-trace", "on"));
  config.retry.baseDelay = 50ms;
  config.retry.maxDelay = 5s;
  config.retry.maxAttempts = 4;
  config.retry.multiplier = 1.5F;
  config.retry.jitter = 0.25F;
  config.retry.retryIdempotentAfterSend = true;
  config.retry.honorRetryAfter = false;
  config.retry.retryStatuses = {http::StatusCodeBadGateway, http::StatusCodeGatewayTimeout};
  config.http2.maxConcurrentStreams = 64;
  config.http2.initialWindowSize = 1U << 20U;
  config.http2.pingInterval = 30s;
  config.withCache(30s).withCacheMaxEntries(10).withCacheMaxRequestSize(1024).withCacheMethods(http::Method::GET |
                                                                                               http::Method::OPTIONS);
  config.followRedirects = false;
  config.keepAlive = false;
  config.addTrailerHeader = false;
  config.httpVersion = HttpVersionMode::Http1_1;
  config.tcpNoDelay = TcpNoDelayMode::Disabled;
#ifdef AERONET_ENABLE_OPENSSL
  config.tlsVerifyPeer = false;
  config.withTlsMinVersion(TLSConfig::TLS_1_3)
      .withTlsMaxVersion(TLSConfig::TLS_1_3)
      .withTlsCaFile("/etc/aeronet/ca.pem")
      .withTlsCaPath("/etc/aeronet/ca")
      .withTlsCipherList("HIGH:!aNULL")
      .withTlsClientCertKeyFile("/etc/aeronet/client.crt", "/etc/aeronet/client.key")
      .withTlsClientCertKeyMemory("CLIENT CERT PEM", "CLIENT KEY PEM");
#endif
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
  config.decompression.enable = false;  // enabled by default, and only valid with a decoder
#endif
  config.decompression.maxCompressedBytes = 1UL << 20U;
  config.decompression.maxExpansionRatio = 10.0;
  config.withRequestCompression(true);
  config.requestCompression.codec.minBytes = 2048;
  config.requestCompression.codec.maxCompressRatio = 0.5F;
  config.requestCompression.codec.contentTypeAllowList.append("application/json");
  config.minCapturedBodySize = 4096;
  config.telemetry.otelEnabled = true;
  config.telemetry.withServiceName("client-service").addDogStatsdTag("env:test").addHttpHeader("x-api-key", "secret");
  const double buckets[]{1.0, 5.0, 10.0};
  config.telemetry.addHistogramBuckets("latency", buckets);
  config.withDefaultAcceptEncoding("gzip").withProxy("http://proxy.local:3128", "/etc/aeronet/proxy-ca.pem");
  return config;
}

// Writes `content` to a file named `fileName` in `dir`.
std::filesystem::path WriteFile(const test::ScopedTempDir& dir, std::string_view fileName, std::string_view content) {
  auto path = dir.dirPath() / fileName;
  std::ofstream(path, std::ios::binary) << content;
  return path;
}

}  // namespace

// ============================================================================
// Field coverage
// ============================================================================
TEST(ClientConfigLoaderTest, EmptySectionKeepsDefaults) {
  const auto defaults = ToJson(HttpClientConfig{});
  EXPECT_EQ(ToJson(LoadClientConfig(R"({"client":{}})", ConfigFormat::json)), defaults);
  EXPECT_EQ(ToJson(LoadClientConfig("{}", ConfigFormat::json)), defaults);
  EXPECT_EQ(ToJson(LoadClientConfig("client: {}\n", ConfigFormat::yaml)), defaults);
}

// Every boolean and number of the configuration, including the ones of a field added later, is read back into itself.
TEST(ClientConfigLoaderTest, EveryScalarFieldRoundTrips) {
  const auto roundTrip = test::MutateScalarsRoundTrip<HttpClientConfig>();
  EXPECT_FALSE(roundTrip.expected.empty());
  EXPECT_EQ(roundTrip.actual, roundTrip.expected);
}

TEST(ClientConfigLoaderTest, NonDefaultConfigRoundTripsJson) {
  const auto original = MakeNonDefaultConfig();
  const auto json = ToJson(original);
  ASSERT_NE(json, ToJson(HttpClientConfig{}));

  const auto loaded = LoadClientConfig(json, ConfigFormat::json);
  EXPECT_EQ(ToJson(loaded), json);
  // A few values read through their accessors, to check that the strings are not only written.
  EXPECT_EQ(loaded.defaultAcceptEncoding(), "gzip");
  EXPECT_EQ(loaded.proxyUrl(), "http://proxy.local:3128");
  EXPECT_EQ(loaded.proxyCaFile(), "/etc/aeronet/proxy-ca.pem");
  EXPECT_EQ(loaded.cache.methods, http::Method::GET | http::Method::OPTIONS);
  EXPECT_EQ(loaded.retry.retryStatuses.size(), 2U);
  EXPECT_EQ(loaded.telemetry.serviceName(), "client-service");
#ifdef AERONET_ENABLE_OPENSSL
  EXPECT_FALSE(loaded.tlsVerifyPeer);
  EXPECT_EQ(loaded.tlsMinVersion, TLSConfig::TLS_1_3);
  EXPECT_EQ(loaded.tlsCaFile(), "/etc/aeronet/ca.pem");
  EXPECT_EQ(loaded.tlsCaPath(), "/etc/aeronet/ca");
  EXPECT_EQ(loaded.tlsCipherList(), "HIGH:!aNULL");
  EXPECT_EQ(loaded.tlsClientCertFile(), "/etc/aeronet/client.crt");
  EXPECT_EQ(loaded.tlsClientKeyFile(), "/etc/aeronet/client.key");
  EXPECT_EQ(loaded.tlsClientCertPem(), "CLIENT CERT PEM");
  EXPECT_EQ(loaded.tlsClientKeyPem(), "CLIENT KEY PEM");
#endif
}

TEST(ClientConfigLoaderTest, NonDefaultConfigRoundTripsYaml) {
  const auto original = MakeNonDefaultConfig();
  const auto yaml = detail::SerializeClientConfig(original, ConfigFormat::yaml);
  EXPECT_TRUE(yaml.starts_with("client:")) << yaml;

  const auto loaded = LoadClientConfig(yaml, ConfigFormat::yaml);
  EXPECT_EQ(ToJson(loaded), ToJson(original));
  EXPECT_EQ(detail::SerializeClientConfig(loaded, ConfigFormat::yaml), yaml);
}

// The documented key names, written by hand.
TEST(ClientConfigLoaderTest, JsonKeys) {
  const auto config = LoadClientConfig(R"({
    "client": {
      "connectTimeout": "5s",
      "requestTimeout": 20000,
      "inactivityTimeout": "1m",
      "keepAliveTimeout": "10s",
      "maxResponseBytes": 1048576,
      "maxRedirects": 1,
      "maxIdleConnectionsPerHost": 2,
      "minReadChunkBytes": 8192,
      "globalHeaders": ["user-agent: test-agent"],
      "retry": {"maxAttempts": 3, "baseDelay": "200ms", "maxDelay": "1s", "retryStatuses": [500, 503]},
      "http2": {"maxConcurrentStreams": 10},
      "cache": {"refreshPeriod": "1m", "maxEntries": 16, "maxRequestSize": 4096, "methods": ["GET"]},
      "followRedirects": false,
      "keepAlive": false,
      "addTrailerHeader": false,
      "httpVersion": "http1.1",
      "tcpNoDelay": "enabled",
      "decompression": {"enable": false},
      "requestCompression": {"encoding": "none", "codec": {"minBytes": 4096}},
      "minCapturedBodySize": 512,
      "telemetry": {"serviceName": "svc"},
      "defaultAcceptEncoding": "identity",
      "proxyUrl": "http://proxy:8080",
      "proxyCaFile": "/etc/proxy-ca.pem"
    }
  })",
                                       ConfigFormat::json);
  EXPECT_EQ(config.connectTimeout, 5s);
  EXPECT_EQ(config.requestTimeout, 20s);
  EXPECT_EQ(config.inactivityTimeout, 1min);
  EXPECT_EQ(config.keepAliveTimeout, 10s);
  EXPECT_EQ(config.maxResponseBytes, 1048576U);
  EXPECT_EQ(config.maxRedirects, 1U);
  EXPECT_EQ(config.maxIdleConnectionsPerHost, 2U);
  EXPECT_EQ(config.minReadChunkBytes, 8192U);
  EXPECT_EQ(config.globalHeaders.fullString(), "user-agent: test-agent");
  EXPECT_EQ(config.retry.maxAttempts, 3U);
  EXPECT_EQ(config.retry.baseDelay, 200ms);
  EXPECT_EQ(config.retry.maxDelay, 1s);
  ASSERT_EQ(config.retry.retryStatuses.size(), 2U);
  EXPECT_TRUE(config.retry.retryStatuses.contains(http::StatusCodeInternalServerError));
  EXPECT_TRUE(config.retry.retryStatuses.contains(http::StatusCodeServiceUnavailable));
  EXPECT_EQ(config.http2.maxConcurrentStreams, 10U);
  EXPECT_EQ(config.cache.refreshPeriod, 1min);
  EXPECT_EQ(config.cache.maxEntries, 16U);
  EXPECT_EQ(config.cache.maxRequestSize, 4096U);
  EXPECT_EQ(config.cache.methods, static_cast<http::MethodBmp>(http::Method::GET));
  EXPECT_FALSE(config.followRedirects);
  EXPECT_FALSE(config.keepAlive);
  EXPECT_FALSE(config.addTrailerHeader);
  EXPECT_EQ(config.httpVersion, HttpVersionMode::Http1_1);
  EXPECT_EQ(config.tcpNoDelay, TcpNoDelayMode::Enabled);
  EXPECT_FALSE(config.decompression.enable);
  EXPECT_FALSE(config.requestCompression.enabled());
  EXPECT_EQ(config.requestCompression.codec.minBytes, 4096U);
  EXPECT_EQ(config.minCapturedBodySize, 512U);
  EXPECT_EQ(config.telemetry.serviceName(), "svc");
  EXPECT_EQ(config.defaultAcceptEncoding(), "identity");
  EXPECT_EQ(config.proxyUrl(), "http://proxy:8080");
  EXPECT_EQ(config.proxyCaFile(), "/etc/proxy-ca.pem");
}

#ifdef AERONET_ENABLE_OPENSSL
TEST(ClientConfigLoaderTest, TlsKeys) {
  const auto config = LoadClientConfig(R"({
    "client": {
      "tlsVerifyPeer": false,
      "tlsMinVersion": "1.2",
      "tlsMaxVersion": "TLS1.3",
      "tlsCaFile": "/etc/ssl/ca.pem",
      "tlsCaPath": "/etc/ssl/certs",
      "tlsCipherList": "ECDHE+AESGCM",
      "tlsClientCertFile": "/etc/ssl/client.crt",
      "tlsClientKeyFile": "/etc/ssl/client.key",
      "tlsClientCertPem": "CERT",
      "tlsClientKeyPem": "KEY"
    }
  })",
                                       ConfigFormat::json);
  EXPECT_FALSE(config.tlsVerifyPeer);
  EXPECT_EQ(config.tlsMinVersion, TLSConfig::TLS_1_2);
  EXPECT_EQ(config.tlsMaxVersion, TLSConfig::TLS_1_3);
  EXPECT_EQ(config.tlsCaFile(), "/etc/ssl/ca.pem");
  EXPECT_STREQ(config.tlsCaFileCStr(), "/etc/ssl/ca.pem");
  EXPECT_EQ(config.tlsCaPath(), "/etc/ssl/certs");
  EXPECT_EQ(config.tlsCipherList(), "ECDHE+AESGCM");
  EXPECT_EQ(config.tlsClientCertFile(), "/etc/ssl/client.crt");
  EXPECT_EQ(config.tlsClientKeyFile(), "/etc/ssl/client.key");
  EXPECT_EQ(config.tlsClientCertPem(), "CERT");
  EXPECT_EQ(config.tlsClientKeyPem(), "KEY");
}

TEST(ClientConfigLoaderTest, InvalidTlsVersionIsAParseError) {
  EXPECT_THROW(LoadClientConfig(R"({"client":{"tlsMinVersion":"1.x"}})", ConfigFormat::json), std::runtime_error);
}
#endif

TEST(ClientConfigLoaderTest, YamlKeys) {
  const auto config = LoadClientConfig(
      "client:\n"
      "  connectTimeout: 3s\n"
      "  maxRedirects: 7\n"
      "  httpVersion: auto\n"
      "  globalHeaders:\n"
      "    - 'user-agent: yaml-agent'\n"
      "    - 'x-custom: 1'\n"
      "  retry:\n"
      "    maxAttempts: 2\n"
      "    retryStatuses: [429]\n"
      "  cache:\n"
      "    refreshPeriod: 1h\n"
      "    methods: [GET, HEAD]\n"
      "  proxyUrl: proxy.local:3128\n",
      ConfigFormat::yaml);
  EXPECT_EQ(config.connectTimeout, 3s);
  EXPECT_EQ(config.maxRedirects, 7U);
  EXPECT_EQ(config.httpVersion, HttpVersionMode::Auto);
  EXPECT_EQ(config.globalHeaders.size(), 2U);
  EXPECT_TRUE(config.globalHeaders.contains("x-custom: 1"));
  EXPECT_EQ(config.retry.maxAttempts, 2U);
  EXPECT_EQ(config.retry.retryStatuses.size(), 1U);
  EXPECT_EQ(config.cache.refreshPeriod, 1h);
  EXPECT_EQ(config.cache.methods, http::Method::GET | http::Method::HEAD);
  EXPECT_EQ(config.proxyUrl(), "proxy.local:3128");
}

// ============================================================================
// Specific value formats
// ============================================================================
TEST(ClientConfigLoaderTest, HttpVersionModes) {
  EXPECT_EQ(LoadClientConfig(R"({"client":{"httpVersion":"auto"}})", ConfigFormat::json).httpVersion,
            HttpVersionMode::Auto);
  EXPECT_EQ(LoadClientConfig(R"({"client":{"httpVersion":"http1.1"}})", ConfigFormat::json).httpVersion,
            HttpVersionMode::Http1_1);
#ifdef AERONET_ENABLE_HTTP2
  EXPECT_EQ(LoadClientConfig(R"({"client":{"httpVersion":"http2"}})", ConfigFormat::json).httpVersion,
            HttpVersionMode::Http2);
#endif
  EXPECT_THROW(LoadClientConfig(R"({"client":{"httpVersion":"http3"}})", ConfigFormat::json), std::runtime_error);
}

TEST(ClientConfigLoaderTest, CacheMethodsAreNames) {
  HttpClientConfig config;
  config.withCache(1s).withCacheMethods(http::Method::OPTIONS | http::Method::GET | http::Method::HEAD);
  const auto json = ToJson(config);
  EXPECT_TRUE(json.contains(R"("methods":["GET","HEAD","OPTIONS"])")) << json;
  EXPECT_EQ(LoadClientConfig(json, ConfigFormat::json).cache.methods, config.cache.methods);

  // An empty list is invalid only when the cache is enabled.
  EXPECT_EQ(LoadClientConfig(R"({"client":{"cache":{"methods":[]}}})", ConfigFormat::json).cache.methods, 0U);
  EXPECT_THROW(LoadClientConfig(R"({"client":{"cache":{"refreshPeriod":"1s","methods":[]}}})", ConfigFormat::json),
               std::invalid_argument);
  // Not a safe method.
  EXPECT_THROW(
      LoadClientConfig(R"({"client":{"cache":{"refreshPeriod":"1s","methods":["POST"]}}})", ConfigFormat::json),
      std::invalid_argument);
  // Method names are case-sensitive (RFC 9110 §9.1).
  EXPECT_THROW(LoadClientConfig(R"({"client":{"cache":{"methods":["get"]}}})", ConfigFormat::json),
               std::invalid_argument);
  EXPECT_THROW(LoadClientConfig(R"({"client":{"cache":{"methods":["FETCH"]}}})", ConfigFormat::json),
               std::invalid_argument);
}

TEST(ClientConfigLoaderTest, CacheForeverRoundTrips) {
  HttpClientConfig config;
  config.withCache(HttpClientConfig::Duration::max());
  for (const auto format : {ConfigFormat::json, ConfigFormat::yaml}) {
    EXPECT_EQ(LoadClientConfig(detail::SerializeClientConfig(config, format), format).cache.refreshPeriod,
              HttpClientConfig::Duration::max());
  }
}

// ============================================================================
// Large values
// ============================================================================
TEST(ClientConfigLoaderTest, LargeValuesRoundTrip) {
  HttpClientConfig config;
  config.globalHeaders.clear();
  for (int headerPos = 0; headerPos < 1000; ++headerPos) {
    config.addGlobalHeader(http::Header("x-header-" + std::to_string(headerPos), std::string(64, 'v')));
  }
  const std::string largeValue(512UL * 1024UL, 'p');
  config.withDefaultAcceptEncoding(largeValue);
#ifdef AERONET_ENABLE_OPENSSL
  config.withTlsClientCertKeyMemory(largeValue, largeValue);
#endif
  for (const auto format : {ConfigFormat::json, ConfigFormat::yaml}) {
    const auto loaded = LoadClientConfig(detail::SerializeClientConfig(config, format), format);
    EXPECT_EQ(loaded.globalHeaders.size(), 1000U);
    EXPECT_EQ(loaded.globalHeaders, config.globalHeaders);
    EXPECT_EQ(loaded.defaultAcceptEncoding(), largeValue);
#ifdef AERONET_ENABLE_OPENSSL
    EXPECT_EQ(loaded.tlsClientKeyPem(), largeValue);
#endif
  }
}

// ============================================================================
// Errors
// ============================================================================
TEST(ClientConfigLoaderTest, InvalidValuesAreRejected) {
  EXPECT_THROW(LoadClientConfig(R"({"client":{"connectTimeout":0}})", ConfigFormat::json), std::invalid_argument);
  EXPECT_THROW(LoadClientConfig(R"({"client":{"retry":{"jitter":2.0}}})", ConfigFormat::json), std::invalid_argument);
  EXPECT_THROW(LoadClientConfig(R"({"client":{"globalHeaders":["no separator"]}})", ConfigFormat::json),
               std::invalid_argument);
}

TEST(ClientConfigLoaderTest, ParseErrors) {
  EXPECT_THROW(LoadClientConfig("{not json}", ConfigFormat::json), std::runtime_error);
  EXPECT_THROW(LoadClientConfig(R"({"client":{"unknownKey":1}})", ConfigFormat::json), std::runtime_error);
  EXPECT_THROW(LoadClientConfig(R"({"client":{"maxRedirects":"many"}})", ConfigFormat::json), std::runtime_error);
  EXPECT_THROW(LoadClientConfig(R"({"client":{"connectTimeout":"soon"}})", ConfigFormat::json), std::runtime_error);
  EXPECT_THROW(LoadClientConfig("client:\n  maxRedirects: [1]\n", ConfigFormat::yaml), std::runtime_error);
  // A server configuration file given by mistake.
  EXPECT_THROW(LoadClientConfig(R"({"server":{"port":8080}})", ConfigFormat::json), std::runtime_error);
}

// The content is a string_view: glaze must not read past its end.
TEST(ClientConfigLoaderTest, ContentIsNotNullTerminated) {
  const std::string_view content = R"({"client":{"maxRedirects":3}}garbage)";
  EXPECT_EQ(LoadClientConfig(content.substr(0, content.find("garbage")), ConfigFormat::json).maxRedirects, 3U);
}

// ============================================================================
// Files
// ============================================================================
TEST(ClientConfigLoaderTest, LoadFromFiles) {
  test::ScopedTempDir dir;
  EXPECT_EQ(LoadClientConfig(WriteFile(dir, "client.json", R"({"client":{"maxRedirects":11}})")).maxRedirects, 11U);
  EXPECT_EQ(LoadClientConfig(WriteFile(dir, "client.yaml", "client:\n  maxRedirects: 12\n")).maxRedirects, 12U);
  EXPECT_EQ(LoadClientConfig(WriteFile(dir, "client.yml", "client:\n  maxRedirects: 13\n")).maxRedirects, 13U);
  EXPECT_THROW(LoadClientConfig(WriteFile(dir, "client.toml", "{}")), std::runtime_error);
  EXPECT_THROW(LoadClientConfig(dir.dirPath() / "missing.json"), std::runtime_error);
}

TEST(ClientConfigLoaderTest, HttpClientFromFile) {
  test::ScopedTempDir dir;
  const HttpClient client(WriteFile(dir, "client.yaml", "client:\n  maxRedirects: 9\n  keepAlive: false\n"));
  EXPECT_EQ(client.config().maxRedirects, 9U);
  EXPECT_FALSE(client.config().keepAlive);

  EXPECT_THROW(HttpClient(dir.dirPath() / "missing.yaml"), std::runtime_error);
  EXPECT_THROW(HttpClient(WriteFile(dir, "invalid.json", R"({"client":{"connectTimeout":0}})")), std::invalid_argument);
}

TEST(ClientConfigLoaderTest, HttpClientDumpAndSaveConfig) {
  auto config = MakeNonDefaultConfig();
  config.telemetry.otelEnabled = false;  // no exporter for this client
  const HttpClient client(std::move(config));
  const auto expected = ToJson(client.config());
  EXPECT_EQ(client.dumpConfig(), expected);
  EXPECT_EQ(ToJson(LoadClientConfig(client.dumpConfig(ConfigFormat::yaml), ConfigFormat::yaml)), expected);

  test::ScopedTempDir dir;
  for (std::string_view fileName : {"saved.json", "saved.yaml", "saved.yml"}) {
    const auto path = dir.dirPath() / fileName;
    client.saveConfig(path);
    EXPECT_EQ(ToJson(LoadClientConfig(path)), expected) << fileName;
    // A client built from the saved file has the same configuration.
    EXPECT_EQ(ToJson(HttpClient(path).config()), expected) << fileName;
  }

  EXPECT_THROW(client.saveConfig(dir.dirPath() / "saved.txt"), std::runtime_error);
  EXPECT_THROW(client.saveConfig(dir.dirPath() / "missing-dir" / "saved.json"), std::runtime_error);
#ifdef AERONET_LINUX
  // Opens, but every write fails (ENOSPC).
  if (std::filesystem::exists("/dev/full")) {
    EXPECT_THROW(detail::WriteConfigFile("/dev/full", std::string(1UL << 16U, 'x')), std::runtime_error);
  }
#endif
}

}  // namespace aeronet
