#pragma once

#include <string_view>

#include "static-string-view-helpers.hpp"

#ifdef AERONET_ENABLE_OPENSSL
#include <openssl/opensslv.h>
#endif
#ifdef AERONET_ENABLE_SPDLOG
#include <spdlog/version.h>
#endif

namespace aeronet {

// Semver of the project as injected by the build system.
constexpr std::string_view version() { return AERONET_PROJECT_VERSION; }

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
  static constexpr std::string_view _sv_logging_prefix = "logging: spdlog ";
  static constexpr std::string_view _sv_newline = "\n  ";
  static constexpr std::string_view _sv_version_macro = AERONET_PROJECT_VERSION;  // same as version()

// TLS section fragment (either full OPENSSL_VERSION_TEXT or disabled)
#ifdef AERONET_ENABLE_OPENSSL
  static constexpr std::string_view _sv_openssl_ver = OPENSSL_VERSION_TEXT;  // e.g. "OpenSSL 3.0.13 30 Jan 2024"
  static constexpr std::string_view _sv_tls_prefix = "tls: ";
  // We choose to keep full OPENSSL_VERSION_TEXT (concise enough) – trimming can be added later if needed.
  using tls_join_t = JoinStringView<_sv_tls_prefix, _sv_openssl_ver>;
  static constexpr std::string_view _sv_tls_section = tls_join_t::value;
#else
  static constexpr std::string_view _sv_tls_disabled = "tls: disabled";
  static constexpr std::string_view _sv_tls_section = _sv_tls_disabled;
#endif

// Logging section fragment
#ifdef AERONET_ENABLE_SPDLOG
  // Convert version numbers at compile time
  static constexpr auto _sv_spdlog_major = IntToStringView<SPDLOG_VER_MAJOR>::value;
  static constexpr auto _sv_spdlog_minor = IntToStringView<SPDLOG_VER_MINOR>::value;
  static constexpr auto _sv_spdlog_patch = IntToStringView<SPDLOG_VER_PATCH>::value;
  static constexpr auto _sv_dot = CharToStringView_v<'.'>;
  using logging_join_t =
      JoinStringView<_sv_logging_prefix, _sv_spdlog_major, _sv_dot, _sv_spdlog_minor, _sv_dot, _sv_spdlog_patch>;
  static constexpr std::string_view _sv_logging_section = logging_join_t::value;
#else
  static constexpr std::string_view _sv_logging_disabled = "logging: disabled";
  static constexpr std::string_view _sv_logging_section = _sv_logging_disabled;
#endif

  // Assemble multiline string (no trailing newline):
  using full_version_join_t =
      JoinStringView<_sv_name, _sv_version_macro, _sv_newline, _sv_tls_section, _sv_newline, _sv_logging_section>;
  return full_version_join_t::value;
}

}  // namespace aeronet