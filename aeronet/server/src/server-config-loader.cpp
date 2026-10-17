#include "aeronet/server-config-loader.hpp"

#include <filesystem>
#include <string>
#include <string_view>

#include "aeronet/aeronet-config.hpp"
#include "aeronet/config-file.hpp"
#include "aeronet/glaze-config.hpp"

namespace aeronet {

namespace detail {

TopLevelConfig ParseConfigFile(const std::filesystem::path& filePath) {
  const auto format = DetectFormat(filePath);
  return ParseConfigString(ReadConfigFile(filePath), format);
}

TopLevelConfig ParseConfigString(std::string_view content, ConfigFormat format) {
  TopLevelConfig config;
  ReadConfig(config, content, format);
  config.server.validate();
  config.router.validate();
  return config;
}

std::string SerializeConfig(const TopLevelConfig& config, ConfigFormat format) { return WriteConfig(config, format); }

}  // namespace detail

HttpServerConfig LoadServerConfig(const std::filesystem::path& filePath) {
  return detail::ParseConfigFile(filePath).server;
}

HttpServerConfig LoadServerConfig(std::string_view content, ConfigFormat format) {
  return detail::ParseConfigString(content, format).server;
}

}  // namespace aeronet
