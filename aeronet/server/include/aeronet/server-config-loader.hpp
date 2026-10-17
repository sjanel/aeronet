#pragma once

#include <filesystem>
#include <string_view>

#include "aeronet/config-file.hpp"  // IWYU pragma: export
#include "aeronet/http-server-config.hpp"

namespace aeronet {

/// Load only HttpServerConfig from a JSON or YAML file (ignores router section).
/// Format is auto-detected from file extension (.json, .yaml, .yml).
/// Throws std::runtime_error on parse or file I/O errors.
HttpServerConfig LoadServerConfig(const std::filesystem::path& filePath);

/// Load only HttpServerConfig from a string buffer with explicit format.
HttpServerConfig LoadServerConfig(std::string_view content, ConfigFormat format);

}  // namespace aeronet
