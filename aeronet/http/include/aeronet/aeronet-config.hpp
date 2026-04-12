#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "aeronet/config-loader.hpp"
#include "aeronet/glaze-config-meta.hpp"
#include "aeronet/glaze-router-meta.hpp"
#include "aeronet/http-server-config.hpp"
#include "aeronet/router-config.hpp"

namespace aeronet {

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

/// Internal: detect config format from file extension. Throws std::runtime_error on unknown extension.
ConfigFormat DetectFormat(const std::filesystem::path& filePath);

}  // namespace detail

}  // namespace aeronet
