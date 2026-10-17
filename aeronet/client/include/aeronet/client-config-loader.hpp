#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "aeronet/config-file.hpp"  // IWYU pragma: export
#include "aeronet/http-client-config.hpp"

namespace aeronet {

/// Load an HttpClientConfig from the 'client' section of a JSON or YAML file, then validate it.
/// Format is auto-detected from file extension (.json, .yaml, .yml).
/// Throws std::runtime_error on parse or file I/O errors, std::invalid_argument on invalid values.
HttpClientConfig LoadClientConfig(const std::filesystem::path& filePath);

/// Load an HttpClientConfig from the 'client' section of a JSON or YAML string with explicit format.
HttpClientConfig LoadClientConfig(std::string_view content, ConfigFormat format);

namespace detail {

/// Internal: serialize a client configuration as the 'client' section of a configuration file.
std::string SerializeClientConfig(const HttpClientConfig& config, ConfigFormat format);

}  // namespace detail

}  // namespace aeronet
