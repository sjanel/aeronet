#pragma once

#include <string>
#include <string_view>

#include "aeronet/static-string-view-helpers.hpp"

#ifdef AERONET_ENABLE_GLAZE
#include <glaze/version.hpp>
#endif

#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/opensslv.h>
#endif
#ifdef AERONET_ENABLE_BROTLI
#include <brotli/decode.h>

#include <charconv>
#include <cstddef>
#include <cstdint>

#include "aeronet/memory-utils.hpp"
#include "aeronet/nchars.hpp"
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

#ifndef AERONET_VERSION_STR
#error "AERONET_VERSION_STR must be defined via build system"
#endif

namespace aeronet {

// Semver of the project as injected by the build system.
constexpr std::string_view version() { return AERONET_VERSION_STR; }

// ---- Compile-time full version string assembly ----
// We build a single static storage string using the JoinStringView utilities so the result
// is available as a constexpr std::string_view without any runtime concatenation cost.
//
// Layout (multiline):
//   aeronet <version>\n
//   tls: <...>\n
//   logging: <...>
//
// Each feature section is determined at compile time via feature macros.
constexpr std::string_view fullVersionStringView() {
  // Base fragments
  static constexpr std::string_view _sv_name = "aeronet ";
  static constexpr std::string_view _sv_newline = "\n  ";
  static constexpr std::string_view _sv_version_macro = AERONET_VERSION_STR;  // same as version()

// TLS section fragment (either full OPENSSL_VERSION_TEXT or disabled)
#ifdef AERONET_ENABLE_OPENSSL
  static constexpr std::string_view _sv_openssl_ver = OPENSSL_VERSION_TEXT;  // e.g. "OpenSSL 3.0.13 30 Jan 2024"
  static constexpr std::string_view _sv_tls_prefix = "tls: ";

  // We choose to keep full OPENSSL_VERSION_TEXT (concise enough) – trimming can be added later if needed.
  using tls_join_t = JoinStringView<_sv_tls_prefix, _sv_openssl_ver>;

  static constexpr std::string_view _sv_tls_section = tls_join_t::value;
#else
  static constexpr std::string_view _sv_tls_section = "tls: disabled";
#endif

// Glaze section fragment (either version or disabled)
#ifdef AERONET_ENABLE_GLAZE
  static constexpr std::string_view _sv_glaze_prefix = "glaze: ";
  static constexpr int _glaze_major = static_cast<int>(glz::version.major);
  static constexpr int _glaze_minor = static_cast<int>(glz::version.minor);
  static constexpr int _glaze_patch = static_cast<int>(glz::version.patch);
  static constexpr auto _sv_glaze_major = IntToStringView<_glaze_major>::value;
  static constexpr auto _sv_glaze_minor = IntToStringView<_glaze_minor>::value;
  static constexpr auto _sv_glaze_patch = IntToStringView<_glaze_patch>::value;
  using glaze_join_t = JoinStringView<_sv_glaze_prefix, _sv_glaze_major, CharToStringView_v<'.'>, _sv_glaze_minor,
                                      CharToStringView_v<'.'>, _sv_glaze_patch>;
  static constexpr std::string_view _sv_glaze_section = glaze_join_t::value;
#else
  static constexpr std::string_view _sv_glaze_section = "glaze: disabled";
#endif

// Logging section fragment
#ifdef AERONET_ENABLE_SPDLOG
  static constexpr std::string_view _sv_logging_prefix = "logging: spdlog ";
  static constexpr auto _sv_spdlog_major = IntToStringView<SPDLOG_VER_MAJOR>::value;
  static constexpr auto _sv_spdlog_minor = IntToStringView<SPDLOG_VER_MINOR>::value;
  static constexpr auto _sv_spdlog_patch = IntToStringView<SPDLOG_VER_PATCH>::value;
  static constexpr auto _sv_dot = CharToStringView_v<'.'>;
  using logging_join_t =
      JoinStringView<_sv_logging_prefix, _sv_spdlog_major, _sv_dot, _sv_spdlog_minor, _sv_dot, _sv_spdlog_patch>;
  static constexpr std::string_view _sv_logging_section = logging_join_t::value;
#else
  static constexpr std::string_view _sv_logging_section = "logging: in-house";
#endif

  // Compression section fragment
#if defined(AERONET_ENABLE_BROTLI) || defined(AERONET_ENABLE_ZLIB) || defined(AERONET_ENABLE_ZSTD)
  static constexpr std::string_view _sv_compression_prefix = "compression: ";
#ifdef AERONET_ENABLE_ZLIB
#ifdef AERONET_ENABLE_ZLIBNG
  static constexpr std::string_view _sv_zlib = "zlib-ng ";
  static constexpr std::string_view _sv_zlib_ver = ZLIBNG_VERSION;  // e.g. "2.2.4"
#else
  static constexpr std::string_view _sv_zlib = "zlib ";
  static constexpr std::string_view _sv_zlib_ver = ZLIB_VERSION;  // e.g. "1.2.11"
#endif
  using zlib_join_t = JoinStringView<_sv_zlib, _sv_zlib_ver>;
  static constexpr std::string_view _sv_zlib_full = zlib_join_t::value;
#endif
#ifdef AERONET_ENABLE_ZSTD
  static constexpr std::string_view _sv_zstd = "zstd ";
  static constexpr std::string_view _sv_zstd_ver = ZSTD_VERSION_STRING;  // e.g. "1.5.6"
  using zstd_join_t = JoinStringView<_sv_zstd, _sv_zstd_ver>;
  static constexpr std::string_view _sv_zstd_full = zstd_join_t::value;
#endif
#ifdef AERONET_ENABLE_BROTLI
  // Brotli version detection is impossible at compile time.
  static constexpr std::string_view _sv_brotli_full = "brotli";
#endif

  // Build a combined list with comma when both present
  [[maybe_unused]] static constexpr std::string_view _sv_comma_space = ", ";

  // Enumerate all combinations (zlib, zstd, brotli) to keep compile-time join logic straightforward.
#if defined(AERONET_ENABLE_ZLIB) && defined(AERONET_ENABLE_ZSTD) && defined(AERONET_ENABLE_BROTLI)
  using compression_join_t = JoinStringView<_sv_compression_prefix, _sv_zlib_full, _sv_comma_space, _sv_zstd_full,
                                            _sv_comma_space, _sv_brotli_full>;
  static constexpr std::string_view _sv_compression_section = compression_join_t::value;
#elif defined(AERONET_ENABLE_ZLIB) && defined(AERONET_ENABLE_ZSTD)
  using compression_join_t = JoinStringView<_sv_compression_prefix, _sv_zlib_full, _sv_comma_space, _sv_zstd_full>;
  static constexpr std::string_view _sv_compression_section = compression_join_t::value;
#elif defined(AERONET_ENABLE_ZLIB) && defined(AERONET_ENABLE_BROTLI)
  using compression_join_t = JoinStringView<_sv_compression_prefix, _sv_zlib_full, _sv_comma_space, _sv_brotli_full>;
  static constexpr std::string_view _sv_compression_section = compression_join_t::value;
#elif defined(AERONET_ENABLE_ZSTD) && defined(AERONET_ENABLE_BROTLI)
  using compression_join_t = JoinStringView<_sv_compression_prefix, _sv_zstd_full, _sv_comma_space, _sv_brotli_full>;
  static constexpr std::string_view _sv_compression_section = compression_join_t::value;
#elifdef AERONET_ENABLE_ZLIB
  using compression_join_t = JoinStringView<_sv_compression_prefix, _sv_zlib_full>;
  static constexpr std::string_view _sv_compression_section = compression_join_t::value;
#elifdef AERONET_ENABLE_ZSTD
  using compression_join_t = JoinStringView<_sv_compression_prefix, _sv_zstd_full>;
  static constexpr std::string_view _sv_compression_section = compression_join_t::value;
#elifdef AERONET_ENABLE_BROTLI
  using compression_join_t = JoinStringView<_sv_compression_prefix, _sv_brotli_full>;
  static constexpr std::string_view _sv_compression_section = compression_join_t::value;
#endif
#else
  static constexpr std::string_view _sv_compression_section = "compression: disabled";
#endif

  // Assemble multiline string (no trailing newline):
  using full_version_join_t =
      JoinStringView<_sv_name, _sv_version_macro, _sv_newline, _sv_glaze_section, _sv_newline, _sv_tls_section,
                     _sv_newline, _sv_logging_section, _sv_newline, _sv_compression_section>;
  return full_version_join_t::value;
}

// Returns a string with the full version info including runtime-detected versions (currently only brotli if enabled).
inline std::string fullVersionWithRuntime() {
  std::string fullVersionStr;

#ifdef AERONET_ENABLE_BROTLI
  static constexpr std::string_view kBaseStr = fullVersionStringView();

  static_assert(kBaseStr.ends_with("brotli"), "brotli should be last because we concatenate the runtime version");

  const uint32_t fullVersion = BrotliDecoderVersion();
  const uint32_t major = fullVersion >> 24;
  const uint32_t minor = (fullVersion >> 12) & 0xFFF;
  const uint32_t patch = fullVersion & 0xFFF;

  // reserve enough space for the base string + brotli version
  fullVersionStr.resize_and_overwrite(kBaseStr.size() + 3U + nchars(major) + nchars(minor) + nchars(patch),
                                      [major, minor, patch](char* data, std::size_t sz) {
                                        char* end = data + sz;

                                        data = Append(kBaseStr, data);
                                        *data++ = ' ';

                                        data = std::to_chars(data, end, major).ptr;
                                        *data++ = '.';
                                        data = std::to_chars(data, end, minor).ptr;
                                        *data++ = '.';
                                        data = std::to_chars(data, end, patch).ptr;

                                        return sz;
                                      });

#else
  fullVersionStr.assign(fullVersionStringView());
#endif

  return fullVersionStr;
}

}  // namespace aeronet