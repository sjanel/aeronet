#pragma once

namespace aeronet {

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

}  // namespace aeronet