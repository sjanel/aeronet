#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

#include "aeronet/features.hpp"
#include "aeronet/ndigits.hpp"
#include "aeronet/static-string-view-helpers.hpp"

#ifdef AERONET_ENABLE_GLAZE
#include <glaze/version.hpp>
#endif

#ifdef AERONET_ENABLE_PROTOBUF
#include <google/protobuf/stubs/common.h>
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/opensslv.h>
#endif

#ifdef AERONET_ENABLE_BROTLI
#include <brotli/decode.h>

#include <cstdint>

#include "aeronet/decimal-writer.hpp"
#include "aeronet/memory-utils-sv.hpp"
#endif

#ifdef AERONET_ENABLE_ZLIB
#include "aeronet/zlib-gateway.hpp"
#endif

#ifdef AERONET_ENABLE_ZSTD
#include <zstd.h>
#endif

#ifdef AERONET_ENABLE_SPDLOG
#include <spdlog/version.h>
#endif

#ifdef AERONET_ENABLE_OPENTELEMETRY
#include <opentelemetry/version.h>
#endif

#ifndef AERONET_VERSION_STR
#error "AERONET_VERSION_STR must be defined via build system"
#endif

namespace aeronet {

// Semver of the project as injected by the build system.
constexpr std::string_view version() { return AERONET_VERSION_STR; }

namespace details {

// Base fragments
inline constexpr std::string_view _sv_name = "aeronet ";
inline constexpr std::string_view _sv_newline = "\n  ";
inline constexpr std::string_view _sv_version_macro = AERONET_VERSION_STR;  // same as version()

// TLS section fragment (either full OPENSSL_VERSION_TEXT or disabled)
#ifdef AERONET_ENABLE_OPENSSL
inline constexpr std::string_view _sv_openssl_ver = OPENSSL_VERSION_TEXT;  // e.g. "OpenSSL 3.0.13 30 Jan 2024"
inline constexpr std::string_view _sv_tls_prefix = "tls: ";

// We choose to keep full OPENSSL_VERSION_TEXT (concise enough) – trimming can be added later if needed.
using tls_join_t = JoinStringView<_sv_tls_prefix, _sv_openssl_ver>;

inline constexpr std::string_view _sv_tls_section = tls_join_t::value;
#else
inline constexpr std::string_view _sv_tls_section = "tls: disabled";
#endif

// Glaze section fragment (either version or disabled)
#ifdef AERONET_ENABLE_GLAZE
inline constexpr std::string_view _sv_glaze_prefix = "glaze: ";
inline constexpr int _glaze_major = static_cast<int>(glz::version.major);
inline constexpr int _glaze_minor = static_cast<int>(glz::version.minor);
inline constexpr int _glaze_patch = static_cast<int>(glz::version.patch);
inline constexpr auto _sv_glaze_major = IntToStringView<_glaze_major>::value;
inline constexpr auto _sv_glaze_minor = IntToStringView<_glaze_minor>::value;
inline constexpr auto _sv_glaze_patch = IntToStringView<_glaze_patch>::value;
inline constexpr std::string_view _sv_glaze_section =
    JoinStringView<_sv_glaze_prefix, _sv_glaze_major, CharToStringView_v<'.'>, _sv_glaze_minor, CharToStringView_v<'.'>,
                   _sv_glaze_patch>::value;
#else
inline constexpr std::string_view _sv_glaze_section = "glaze: disabled";
#endif

// Protobuf section fragment: version of the C++ runtime headers (e.g. 5.29.3 for protobuf v29.3), with the runtime.
#ifdef AERONET_ENABLE_PROTOBUF
inline constexpr std::string_view _sv_protobuf_prefix = "protobuf: ";
inline constexpr auto _sv_protobuf_major = IntToStringView<GOOGLE_PROTOBUF_VERSION / 1000000>::value;
inline constexpr auto _sv_protobuf_minor = IntToStringView<GOOGLE_PROTOBUF_VERSION / 1000 % 1000>::value;
inline constexpr auto _sv_protobuf_patch = IntToStringView<GOOGLE_PROTOBUF_VERSION % 1000>::value;
// Empty for a stable release, "-rc1" for a release candidate.
// NOLINTNEXTLINE(readability-redundant-string-init)
inline constexpr std::string_view _sv_protobuf_suffix = GOOGLE_PROTOBUF_VERSION_SUFFIX;
#ifdef AERONET_PROTOBUF_LITE
inline constexpr std::string_view _sv_protobuf_runtime = " (lite)";
#else
inline constexpr std::string_view _sv_protobuf_runtime{};
#endif
inline constexpr std::string_view _sv_protobuf_section =
    JoinStringView<_sv_protobuf_prefix, _sv_protobuf_major, CharToStringView_v<'.'>, _sv_protobuf_minor,
                   CharToStringView_v<'.'>, _sv_protobuf_patch, _sv_protobuf_suffix, _sv_protobuf_runtime>::value;
#else
inline constexpr std::string_view _sv_protobuf_section = "protobuf: disabled";
#endif

// Logging section fragment
#ifdef AERONET_ENABLE_SPDLOG
inline constexpr std::string_view _sv_logging_prefix = "logging: spdlog ";
inline constexpr auto _sv_spdlog_major = IntToStringView<SPDLOG_VER_MAJOR>::value;
inline constexpr auto _sv_spdlog_minor = IntToStringView<SPDLOG_VER_MINOR>::value;
inline constexpr auto _sv_spdlog_patch = IntToStringView<SPDLOG_VER_PATCH>::value;
inline constexpr auto _sv_dot = CharToStringView_v<'.'>;
inline constexpr std::string_view _sv_logging_section =
    JoinStringView<_sv_logging_prefix, _sv_spdlog_major, _sv_dot, _sv_spdlog_minor, _sv_dot, _sv_spdlog_patch>::value;
#else
inline constexpr std::string_view _sv_logging_section = "logging: in-house";
#endif

// OpenTelemetry section fragment (either version or disabled)
#ifdef AERONET_ENABLE_OPENTELEMETRY
inline constexpr std::string_view _sv_opentelemetry_prefix = "opentelemetry: ";
inline constexpr std::string_view _sv_opentelemetry_ver = OPENTELEMETRY_VERSION;  // e.g. "1.30.0"
inline constexpr std::string_view _sv_opentelemetry_section =
    JoinStringView<_sv_opentelemetry_prefix, _sv_opentelemetry_ver>::value;
#else
inline constexpr std::string_view _sv_opentelemetry_section = "opentelemetry: disabled";
#endif

// Features section fragment: the compile-time flags that have no section of their own, as a comma separated list.
struct FeatureFlag {
  std::string_view name;
  bool enabled;
};

inline constexpr FeatureFlag _features[]{
    {"http-server", httpServerEnabled()}, {"http-client", httpClientEnabled()},       {"http2", http2Enabled()},
    {"websocket", webSocketEnabled()},    {"async-handlers", asyncHandlersEnabled()},
};

inline constexpr std::string_view _sv_features_prefix = "features: ";
inline constexpr std::string_view _sv_features_none = "none";
inline constexpr std::string_view _sv_features_sep = ", ";

// Writes the section into dst (when not null) and returns its length.
consteval std::size_t WriteFeaturesSection(char* dst) {
  std::size_t len = 0;
  const auto append = [dst, &len](std::string_view sv) {
    for (const char ch : sv) {
      if (dst != nullptr) {
        dst[len] = ch;
      }
      ++len;
    }
  };
  append(_sv_features_prefix);
  bool first = true;
  for (const FeatureFlag& feature : _features) {
    if (feature.enabled) {
      if (!first) {
        append(_sv_features_sep);
      }
      append(feature.name);
      first = false;
    }
  }
  if (first) {
    append(_sv_features_none);
  }
  return len;
}

inline constexpr std::size_t _features_section_len = WriteFeaturesSection(nullptr);

// Null terminated static storage of the section
inline constexpr std::array<char, _features_section_len + 1U> _features_section_chars = [] consteval {
  std::array<char, _features_section_len + 1U> chars{};
  WriteFeaturesSection(chars.data());
  return chars;
}();

inline constexpr std::string_view _sv_features_section{_features_section_chars.data(), _features_section_len};

// Compression section fragment
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
inline constexpr std::string_view _sv_compression_prefix = "compression: ";
#ifdef AERONET_ENABLE_ZLIB
#ifdef AERONET_ENABLE_ZLIBNG
inline constexpr std::string_view _sv_zlib = "zlib-ng ";
inline constexpr std::string_view _sv_zlib_ver = ZLIBNG_VERSION;  // e.g. "2.2.4"
#else
inline constexpr std::string_view _sv_zlib = "zlib ";
inline constexpr std::string_view _sv_zlib_ver = ZLIB_VERSION;  // e.g. "1.2.11"
#endif
inline constexpr std::string_view _sv_zlib_full = JoinStringView<_sv_zlib, _sv_zlib_ver>::value;
#endif
#ifdef AERONET_ENABLE_ZSTD
inline constexpr std::string_view _sv_zstd = "zstd ";
inline constexpr std::string_view _sv_zstd_ver = ZSTD_VERSION_STRING;  // e.g. "1.5.6"
inline constexpr std::string_view _sv_zstd_full = JoinStringView<_sv_zstd, _sv_zstd_ver>::value;
#endif
#ifdef AERONET_ENABLE_BROTLI
// Brotli version detection is impossible at compile time.
inline constexpr std::string_view _sv_brotli_full = "brotli";
#endif

// Build a combined list with comma when both present
[[maybe_unused]] inline constexpr std::string_view _sv_comma_space = ", ";

// Enumerate all combinations (zlib, zstd, brotli) to keep compile-time join logic straightforward.
#if defined(AERONET_ENABLE_ZLIB) && defined(AERONET_ENABLE_ZSTD) && defined(AERONET_ENABLE_BROTLI)
using compression_join_t = JoinStringView<_sv_compression_prefix, _sv_zlib_full, _sv_comma_space, _sv_zstd_full,
                                          _sv_comma_space, _sv_brotli_full>;
inline constexpr std::string_view _sv_compression_section = compression_join_t::value;
#elif defined(AERONET_ENABLE_ZLIB) && defined(AERONET_ENABLE_ZSTD)
using compression_join_t = JoinStringView<_sv_compression_prefix, _sv_zlib_full, _sv_comma_space, _sv_zstd_full>;
inline constexpr std::string_view _sv_compression_section = compression_join_t::value;
#elif defined(AERONET_ENABLE_ZLIB) && defined(AERONET_ENABLE_BROTLI)
using compression_join_t = JoinStringView<_sv_compression_prefix, _sv_zlib_full, _sv_comma_space, _sv_brotli_full>;
inline constexpr std::string_view _sv_compression_section = compression_join_t::value;
#elif defined(AERONET_ENABLE_ZSTD) && defined(AERONET_ENABLE_BROTLI)
using compression_join_t = JoinStringView<_sv_compression_prefix, _sv_zstd_full, _sv_comma_space, _sv_brotli_full>;
inline constexpr std::string_view _sv_compression_section = compression_join_t::value;
#elifdef AERONET_ENABLE_ZLIB
using compression_join_t = JoinStringView<_sv_compression_prefix, _sv_zlib_full>;
inline constexpr std::string_view _sv_compression_section = compression_join_t::value;
#elifdef AERONET_ENABLE_ZSTD
using compression_join_t = JoinStringView<_sv_compression_prefix, _sv_zstd_full>;
inline constexpr std::string_view _sv_compression_section = compression_join_t::value;
#elifdef AERONET_ENABLE_BROTLI
using compression_join_t = JoinStringView<_sv_compression_prefix, _sv_brotli_full>;
inline constexpr std::string_view _sv_compression_section = compression_join_t::value;
#endif
#else
inline constexpr std::string_view _sv_compression_section = "compression: disabled";
#endif

}  // namespace details

// ---- Compile-time full version string assembly ----
// We build a single static storage string using the JoinStringView utilities so the result
// is available as a constexpr std::string_view without any runtime concatenation cost.
//
// Layout (multiline):
//   aeronet <version>\n
//   glaze: <...>\n
//   protobuf: <...>\n
//   tls: <...>\n
//   logging: <...>
//   opentelemetry: <...>
//   features: <...>
//   compression: <...>
//
// Each feature section is determined at compile time via feature macros.
constexpr std::string_view fullVersionStringView() {  // Assemble multiline string (no trailing newline):
  using namespace details;

  using full_version_join_t = JoinStringView<_sv_name, _sv_version_macro, _sv_newline, _sv_glaze_section, _sv_newline,
                                             _sv_protobuf_section, _sv_newline, _sv_tls_section, _sv_newline,
                                             _sv_logging_section, _sv_newline, _sv_opentelemetry_section, _sv_newline,
                                             _sv_features_section, _sv_newline, _sv_compression_section>;

  return full_version_join_t::value;
}

// Returns a string with the full version info including runtime-detected versions (currently only brotli if enabled).
inline std::string fullVersionWithRuntime() {
  std::string fullVersionStr;

#ifdef AERONET_ENABLE_BROTLI
  static constexpr std::string_view kBaseStr = fullVersionStringView();

  static_assert(kBaseStr.ends_with("brotli"), "brotli should be last because we concatenate the runtime version");

  const uint32_t fullVersion = BrotliDecoderVersion();
  const uint8_t major = static_cast<uint8_t>(fullVersion >> 24U);
  const uint16_t minor = static_cast<uint16_t>((fullVersion >> 12U) & 0xFFFU);
  const uint16_t patch = static_cast<uint16_t>(fullVersion & 0xFFFU);

  const auto nDigitsMajor = ndigits(major);
  const auto nDigitsMinor = ndigits(minor);
  const auto nDigitsPatch = ndigits(patch);

  // reserve enough space for the base string + brotli version
  fullVersionStr.resize_and_overwrite(
      kBaseStr.size() + 3U + nDigitsMajor + nDigitsMinor + nDigitsPatch,
      [major, minor, patch, nDigitsMajor, nDigitsMinor, nDigitsPatch](char* data, std::size_t sz) {
        data = AppendFixed<kBaseStr>(data);
        *data++ = ' ';

        data = WriteUInt(data, major, nDigitsMajor);
        *data++ = '.';
        data = WriteUInt(data, minor, nDigitsMinor);
        *data++ = '.';
        data = WriteUInt(data, patch, nDigitsPatch);

        return sz;
      });

#else
  fullVersionStr.assign(fullVersionStringView());
#endif

  return fullVersionStr;
}

}  // namespace aeronet