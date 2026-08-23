# TLS and HTTP/2

TLS and HTTP/2 are opt-in modules. Enable them at CMake configure time so applications that only need HTTP/1.1 do not pay for their dependencies or binary surface.

## TLS

Set `AERONET_ENABLE_OPENSSL=ON` and configure a certificate and key in `HttpServerConfig`. aeronet supports TLS configuration, ALPN, hot certificate reload, session tickets, mTLS-related configuration, and Linux-specific kTLS paths where the platform supports them.

Read [TLS features](../FEATURES.md#tls-features) before deploying, especially the certificate, ALPN, handshake-metrics, and hot-reload sections. The [TLS example](../../examples/tls-ktls.cpp) is a complete starting point.

```cpp
HttpServerConfig config;
config.withPort(443)
    .withTlsCertKey("/run/tls/tls.crt", "/run/tls/tls.key")
    .withTlsHandshakeTimeout(std::chrono::seconds{10});
config.tls.withTlsMinVersion("TLS1.2");
```

For strict mTLS, add a trusted client certificate and call `withTlsRequireClientCert()`; merely requesting a certificate is not the same as requiring it. TLS session ticket rotation, SNI mappings, cipher policy, ALPN failure behavior, and kTLS policy are listed in the [server configuration reference](../reference/server-configuration.md#tls-configuration).

## HTTP/2

Set `AERONET_ENABLE_HTTP2=ON`, then enable the feature in `Http2Config`. HTTP/2 can be used over TLS through ALPN or in clear text through h2c when that is appropriate for your environment.

The same request-handler model is used for HTTP/1.1 and HTTP/2, so handlers can inspect protocol-specific properties only where necessary. See [HTTP/2](../FEATURES.md#http2-rfc-9113) for configuration, h2c, stream behavior, and test commands. A complete [HTTP/2 example](../../examples/http2.cpp) demonstrates both TLS and h2c modes.

```cpp
HttpServerConfig config;
Http2Config http2;
http2.withMaxConcurrentStreams(128)
    .withPingInterval(std::chrono::seconds{30})
    .withEnableH2c(false)
    .withEnableH2cUpgrade(false);

config.withTlsAlpnProtocols({"h2", "http/1.1"}).withHttp2(http2);
```

!!! warning
    HTTP/2 flow control, concurrent stream limits, and TLS certificate management are production settings. Set them deliberately for your workload instead of relying on benchmark-oriented values.
