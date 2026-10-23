#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "aeronet/config-file.hpp"  // IWYU pragma: export
#include "aeronet/http-server-config.hpp"
#include "aeronet/router-config.hpp"

namespace aeronet {

/// Load only HttpServerConfig from a JSON or YAML file (ignores router section).
/// Format is auto-detected from file extension (.json, .yaml, .yml).
/// Throws std::runtime_error on parse or file I/O errors.
HttpServerConfig LoadServerConfig(const std::filesystem::path& filePath);

/// Load only HttpServerConfig from a string buffer with explicit format.
HttpServerConfig LoadServerConfig(std::string_view content, ConfigFormat format);

/// Internal top-level configuration combining server and router settings.
/// Not part of the public API - use server filepath constructors or LoadServerConfig instead.
struct TopLevelConfig {
  HttpServerConfig server;
  RouterConfig router;
};

namespace detail {

/// Internal: parse full config from a JSON/YAML file (format auto-detected from extension).
TopLevelConfig ParseConfigFile(const std::filesystem::path& filePath);

/// Internal: parse full config from a string buffer with explicit format.
TopLevelConfig ParseConfigString(std::string_view content, ConfigFormat format);

/// Internal: serialize full config to a string.
std::string SerializeConfig(const TopLevelConfig& config, ConfigFormat format);

}  // namespace detail

}  // namespace aeronet
