#include "aeronet/client-config-loader.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/config-file.hpp"
#include "aeronet/glaze-config.hpp"
#include "aeronet/http-client-config-glaze.hpp"  // IWYU pragma: keep (glz::meta of HttpClientConfig)
#include "aeronet/http-client-config.hpp"
#include "aeronet/http-client.hpp"

namespace aeronet {

namespace detail {

// Layout of a client configuration file (in a named namespace: glaze reflects types with linkage only). The section
// leaves room for other ones in the future, and makes a server configuration file given by mistake fail to load.
// NOLINTNEXTLINE(misc-use-internal-linkage)
struct ClientTopLevelConfig {
  HttpClientConfig client;
};

// Same layout as ClientTopLevelConfig, to write a configuration without copying it.
// NOLINTNEXTLINE(misc-use-internal-linkage)
struct ClientTopLevelConfigView {
  const HttpClientConfig* client;
};

}  // namespace detail

HttpClientConfig LoadClientConfig(const std::filesystem::path& filePath) {
  const auto format = detail::DetectFormat(filePath);
  return LoadClientConfig(detail::ReadConfigFile(filePath), format);
}

HttpClientConfig LoadClientConfig(std::string_view content, ConfigFormat format) {
  detail::ClientTopLevelConfig config;
  detail::ReadConfig(config, content, format);
  config.client.validate();
  return std::move(config.client);
}

namespace detail {

std::string SerializeClientConfig(const HttpClientConfig& config, ConfigFormat format) {
  return WriteConfig(ClientTopLevelConfigView{&config}, format);
}

}  // namespace detail

HttpClient::HttpClient(const std::filesystem::path& configPath) : HttpClient(LoadClientConfig(configPath)) {}

std::string HttpClient::dumpConfig(ConfigFormat format) const { return detail::SerializeClientConfig(_config, format); }

void HttpClient::saveConfig(const std::filesystem::path& filePath) const {
  detail::WriteConfigFile(filePath, dumpConfig(detail::DetectFormat(filePath)));
}

}  // namespace aeronet
