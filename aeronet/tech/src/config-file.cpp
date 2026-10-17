#include "aeronet/config-file.hpp"

#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace aeronet::detail {

ConfigFormat DetectFormat(const std::filesystem::path& filePath) {
  const auto ext = filePath.extension().string();
  if (ext == ".yaml" || ext == ".yml") {
    return ConfigFormat::yaml;
  }
  if (ext == ".json") {
    return ConfigFormat::json;
  }
  throw std::runtime_error("Cannot detect config format from extension '" + ext +
                           "'. Expected .json, .yaml, or .yml for file: " + filePath.string());
}

std::string ReadConfigFile(const std::filesystem::path& filePath) {
  std::ifstream ifs(filePath, std::ios::binary);
  if (!ifs) {
    throw std::runtime_error("Failed to open config file: " + filePath.string());
  }
  std::ostringstream oss;
  oss << ifs.rdbuf();
  return std::move(oss).str();
}

void WriteConfigFile(const std::filesystem::path& filePath, std::string_view content) {
  std::ofstream ofs(filePath, std::ios::binary | std::ios::trunc);
  if (!ofs) {
    throw std::runtime_error("Failed to open file for writing: " + filePath.string());
  }
  ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
  if (!ofs.flush()) {
    throw std::runtime_error("Failed to write config file: " + filePath.string());
  }
}

}  // namespace aeronet::detail
