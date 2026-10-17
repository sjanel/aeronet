#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace aeronet {

/// Config file format.
enum class ConfigFormat : uint8_t { json, yaml };

namespace detail {

/// Internal: detect config format from file extension (.json, .yaml, .yml). Throws std::runtime_error on unknown
/// extension.
ConfigFormat DetectFormat(const std::filesystem::path& filePath);

/// Internal: read the whole content of a config file. Throws std::runtime_error if it cannot be opened.
std::string ReadConfigFile(const std::filesystem::path& filePath);

/// Internal: write content to a config file, replacing it. Throws std::runtime_error if it cannot be written.
void WriteConfigFile(const std::filesystem::path& filePath, std::string_view content);

}  // namespace detail

}  // namespace aeronet
