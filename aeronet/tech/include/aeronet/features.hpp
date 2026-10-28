#pragma once

namespace aeronet {

#ifdef AERONET_ENABLE_HTTP_SERVER
constexpr bool httpServerEnabled() { return true; }
#else
constexpr bool httpServerEnabled() { return false; }
#endif

#ifdef AERONET_ENABLE_HTTP_CLIENT
constexpr bool httpClientEnabled() { return true; }
#else
constexpr bool httpClientEnabled() { return false; }
#endif

#ifdef AERONET_ENABLE_SPDLOG
constexpr bool spdLogEnabled() { return true; }
#else
constexpr bool spdLogEnabled() { return false; }
#endif

#ifdef AERONET_ENABLE_OPENSSL
constexpr bool openSslEnabled() { return true; }
#else
constexpr bool openSslEnabled() { return false; }
#endif

#ifdef AERONET_ENABLE_HTTP2
constexpr bool http2Enabled() { return true; }
#else
constexpr bool http2Enabled() { return false; }
#endif

#ifdef AERONET_ENABLE_ZLIB
constexpr bool zlibEnabled() { return true; }
#else
constexpr bool zlibEnabled() { return false; }
#endif

#ifdef AERONET_ENABLE_ZSTD
constexpr bool zstdEnabled() { return true; }
#else
constexpr bool zstdEnabled() { return false; }
#endif

#ifdef AERONET_ENABLE_BROTLI
constexpr bool brotliEnabled() { return true; }
#else
constexpr bool brotliEnabled() { return false; }
#endif

#ifdef AERONET_ENABLE_OPENTELEMETRY
constexpr bool openTelemetryEnabled() { return true; }
#else
constexpr bool openTelemetryEnabled() { return false; }
#endif

#ifdef AERONET_ENABLE_PROTOBUF
constexpr bool protobufEnabled() { return true; }
#else
constexpr bool protobufEnabled() { return false; }
#endif

#ifdef AERONET_ENABLE_WEBSOCKET
constexpr bool webSocketEnabled() { return true; }
#else
constexpr bool webSocketEnabled() { return false; }
#endif

#ifdef AERONET_ENABLE_ASYNC_HANDLERS
constexpr bool asyncHandlersEnabled() { return true; }
#else
constexpr bool asyncHandlersEnabled() { return false; }
#endif

}  // namespace aeronet