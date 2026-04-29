#include "aeronet/config-loader.hpp"

#include <filesystem>
#include <fstream>
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>
#include <ios>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/aeronet-config.hpp"

namespace aeronet {

namespace {

std::string ReadFileContents(const std::filesystem::path& filePath) {
  std::ifstream ifs(filePath, std::ios::binary);
  if (!ifs) {
    throw std::runtime_error("Failed to open config file: " + filePath.string());
  }
  std::ostringstream oss;
  oss << ifs.rdbuf();
  return oss.str();
}

}  // namespace

namespace detail {

ConfigFormat DetectFormat(const std::filesystem::path& filePath) {
  auto ext = filePath.extension().string();
  if (ext == ".yaml" || ext == ".yml") {
    return ConfigFormat::yaml;
  }
  if (ext == ".json") {
    return ConfigFormat::json;
  }
  throw std::runtime_error("Cannot detect config format from extension '" + ext +
                           "'. Expected .json, .yaml, or .yml for file: " + filePath.string());
}

TopLevelConfig ParseConfigFile(const std::filesystem::path& filePath) {
  auto format = DetectFormat(filePath);
  auto content = ReadFileContents(filePath);
  return ParseConfigString(content, format);
}

TopLevelConfig ParseConfigString(std::string_view content, ConfigFormat format) {
  TopLevelConfig config;
  glz::error_ctx ec;
  if (format == ConfigFormat::json) {
    ec = glz::read_json(config, content);
  } else {
    ec = glz::read<glz::opts{.format = glz::YAML}>(config, content);
  }
  if (ec) {
    throw std::runtime_error(std::string("Config parse error: ") + glz::format_error(ec, content));
  }
  config.server.validate();
  config.router.validate();
  return config;
}

std::string SerializeConfig(const TopLevelConfig& config, ConfigFormat format) {
  auto result =
      (format == ConfigFormat::json) ? glz::write_json(config) : glz::write<glz::opts{.format = glz::YAML}>(config);
  if (!result) {
    throw std::invalid_argument("Config serialization failed: " + glz::format_error(result.error()));
  }
  return std::move(result.value());
}

}  // namespace detail

HttpServerConfig LoadServerConfig(const std::filesystem::path& filePath) {
  return detail::ParseConfigFile(filePath).server;
}

HttpServerConfig LoadServerConfig(std::string_view content, ConfigFormat format) {
  return detail::ParseConfigString(content, format).server;
}

}  // namespace aeronet
