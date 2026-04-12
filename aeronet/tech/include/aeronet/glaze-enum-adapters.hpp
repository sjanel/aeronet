#pragma once

#ifdef AERONET_ENABLE_GLAZE

#include <glaze/glaze.hpp>

#include "aeronet/builtin-probes-config.hpp"
#include "aeronet/direct-compression-mode.hpp"
#include "aeronet/encoding.hpp"
#include "aeronet/http-method.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/router-config.hpp"
#include "aeronet/tcp-no-delay-mode.hpp"
#include "aeronet/tls-config.hpp"
#include "aeronet/zerocopy-mode.hpp"

// ============================================================================
// Glaze enum adapters for aeronet enums
// ============================================================================

template <>
struct glz::meta<aeronet::TcpNoDelayMode> {
  using enum aeronet::TcpNoDelayMode;
  static constexpr auto value = glz::enumerate("auto", Auto, "disabled", Disabled, "enabled", Enabled);
};

template <>
struct glz::meta<aeronet::ZerocopyMode> {
  using enum aeronet::ZerocopyMode;
  static constexpr auto value =
      glz::enumerate("disabled", Disabled, "opportunistic", Opportunistic, "enabled", Enabled);
};

template <>
struct glz::meta<aeronet::DirectCompressionMode> {
  using enum aeronet::DirectCompressionMode;
  static constexpr auto value = glz::enumerate("auto", Auto, "off", Off, "on", On);
};

template <>
struct glz::meta<aeronet::TLSConfig::KtlsMode> {
  using enum aeronet::TLSConfig::KtlsMode;
  static constexpr auto value =
      glz::enumerate("disabled", Disabled, "opportunistic", Opportunistic, "enabled", Enabled, "required", Required);
};

template <>
struct glz::meta<aeronet::TLSConfig::CipherPolicy> {
  using enum aeronet::TLSConfig::CipherPolicy;
  static constexpr auto value =
      glz::enumerate("default", Default, "modern", Modern, "compatibility", Compatibility, "legacy", Legacy);
};

template <>
struct glz::meta<aeronet::HttpServerConfig::TraceMethodPolicy> {
  using enum aeronet::HttpServerConfig::TraceMethodPolicy;
  static constexpr auto value = glz::enumerate("disabled", Disabled, "enabledPlainAndTLS", EnabledPlainAndTLS,
                                               "enabledPlainOnly", EnabledPlainOnly);
};

template <>
struct glz::meta<aeronet::Encoding> {
  using enum aeronet::Encoding;
  static constexpr auto value = glz::enumerate("zstd", zstd, "br", br, "gzip", gzip, "deflate", deflate, "none", none);
};

template <>
struct glz::meta<aeronet::BuiltinProbesConfig::ContentType> {
  using enum aeronet::BuiltinProbesConfig::ContentType;
  static constexpr auto value = glz::enumerate("text/plain; charset=utf-8", TextPlainUtf8);
};

template <>
struct glz::meta<aeronet::RouterConfig::TrailingSlashPolicy> {
  using enum aeronet::RouterConfig::TrailingSlashPolicy;
  static constexpr auto value = glz::enumerate("strict", Strict, "normalize", Normalize, "redirect", Redirect);
};

#endif  // AERONET_ENABLE_GLAZE
