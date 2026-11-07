#pragma once

#include <array>
#include <cstddef>
#include <span>
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
inline constexpr std::string_view _sv_glaze_major = IntToStringView<_glaze_major>::value;
inline constexpr std::string_view _sv_glaze_minor = IntToStringView<_glaze_minor>::value;
inline constexpr std::string_view _sv_glaze_patch = IntToStringView<_glaze_patch>::value;
inline constexpr std::string_view _sv_glaze_section =
    JoinStringView<_sv_glaze_prefix, _sv_glaze_major, CharToStringView_v<'.'>, _sv_glaze_minor, CharToStringView_v<'.'>,
                   _sv_glaze_patch>::value;
#else
inline constexpr std::string_view _sv_glaze_section = "glaze: disabled";
#endif

// Protobuf section fragment: version of the C++ runtime headers (e.g. 5.29.3 for protobuf v29.3), with the runtime.
#ifdef AERONET_ENABLE_PROTOBUF
inline constexpr std::string_view _sv_protobuf_prefix = "protobuf: ";
inline constexpr std::string_view _sv_protobuf_major = IntToStringView<GOOGLE_PROTOBUF_VERSION / 1000000>::value;
inline constexpr std::string_view _sv_protobuf_minor = IntToStringView<GOOGLE_PROTOBUF_VERSION / 1000 % 1000>::value;
inline constexpr std::string_view _sv_protobuf_patch = IntToStringView<GOOGLE_PROTOBUF_VERSION % 1000>::value;
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

// ---- Generic comma separated list section ----
// Used by every section that is a list of optional items (features, compression codecs).
// Only the enabled items are written, in declaration order; when none is enabled the section is prefix + empty text.
struct ListItem {
  std::string_view name;
  bool enabled;
};

inline constexpr std::string_view _sv_list_sep = ", ";

struct ListSpec {
  std::string_view prefix;
  std::span<const ListItem> items;
  std::string_view whenEmpty;

  // Writes the section into dst (when not null) and returns its length.
  consteval std::size_t write(char* dst) const {
    std::size_t len = 0;
    const auto append = [dst, &len](std::string_view sv) {
      for (const char ch : sv) {
        if (dst != nullptr) {
          dst[len] = ch;
        }
        ++len;
      }
    };
    append(prefix);
    bool first = true;
    for (const ListItem& item : items) {
      if (item.enabled) {
        if (!first) {
          append(_sv_list_sep);
        }
        append(item.name);
        first = false;
      }
    }
    if (first) {
      append(whenEmpty);
    }
    return len;
  }
};

// Writes the section of spec into an array of its exact length (N = spec.write(nullptr)).
template <std::size_t N>
consteval std::array<char, N> ListSectionChars(const ListSpec& spec) {
  std::array<char, N> chars{};
  spec.write(chars.data());
  return chars;
}

// Static storage of the section, exposed as a string_view.
// No lambda here: inside one, MSVC takes the pointer template parameter for a ListSpec object (C2819).
template <const ListSpec* Spec>
struct ListSection {
  static constexpr std::size_t kLen = Spec->write(nullptr);

  static constexpr std::array<char, kLen> kChars = ListSectionChars<kLen>(*Spec);

  static constexpr std::string_view value{kChars.data(), kLen};
};

// Features section fragment: the compile-time flags that have no section of their own.
inline constexpr ListItem _features[]{
    {"http-server", httpServerEnabled()}, {"http-client", httpClientEnabled()},       {"http2", http2Enabled()},
    {"websocket", webSocketEnabled()},    {"async-handlers", asyncHandlersEnabled()}, {"html", htmlEnabled()},
};

inline constexpr std::string_view _sv_features_prefix = "features: ";
inline constexpr std::string_view _sv_features_none = "none";

inline constexpr ListSpec _features_spec{_sv_features_prefix, _features, _sv_features_none};

inline constexpr std::string_view _sv_features_section = ListSection<&_features_spec>::value;

// Compression section fragment
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
// Brotli version detection is impossible at compile time (it is appended at runtime, see fullVersionWithRuntime).
inline constexpr std::string_view _sv_brotli_full = "brotli";
#endif

// One entry per codec, in display order. brotli must stay last: fullVersionWithRuntime appends its version.
// Adding a codec only needs its version fragment above and one entry here.
inline constexpr ListItem _compression_codecs[]{
#ifdef AERONET_ENABLE_ZLIB
    {_sv_zlib_full, true},
#else
    {std::string_view{}, false},
#endif
#ifdef AERONET_ENABLE_ZSTD
    {_sv_zstd_full, true},
#else
    {std::string_view{}, false},
#endif
#ifdef AERONET_ENABLE_BROTLI
    {_sv_brotli_full, true},
#else
    {std::string_view{}, false},
#endif
};

inline constexpr std::string_view _sv_compression_prefix = "compression: ";
inline constexpr std::string_view _sv_compression_none = "disabled";

inline constexpr ListSpec _compression_spec{_sv_compression_prefix, _compression_codecs, _sv_compression_none};

inline constexpr std::string_view _sv_compression_section = ListSection<&_compression_spec>::value;

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
      [major, minor, patch, nDigitsMajor, nDigitsMinor, nDigitsPatch](char* pData, std::size_t sz) {
        pData = AppendFixed<kBaseStr>(pData);
        *pData++ = ' ';

        pData = WriteUInt(pData, major, nDigitsMajor);
        *pData++ = '.';
        pData = WriteUInt(pData, minor, nDigitsMinor);
        *pData++ = '.';
        pData = WriteUInt(pData, patch, nDigitsPatch);

        return sz;
      });

#else
  fullVersionStr.assign(fullVersionStringView());
#endif

  return fullVersionStr;
}

}  // namespace aeronet