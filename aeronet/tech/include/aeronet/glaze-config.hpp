#pragma once

#include <cassert>
#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "aeronet/config-file.hpp"

namespace aeronet::detail {

// Internal: read a JSON / YAML configuration into 'config'. Keys absent from 'content' keep their current value, and
// an unknown key is an error. Throws std::runtime_error on parse errors.
template <class T>
void ReadConfig(T& config, std::string_view content, ConfigFormat format) {
  glz::error_ctx ec;
  // `content` is a string_view (not necessarily a terminated std::string), so tell glaze to honour the end pointer
  // strictly rather than assume a '\0'.
  if (format == ConfigFormat::json) {
    ec = glz::read<glz::opts{.null_terminated = false}>(config, content);
  } else {
    assert(format == ConfigFormat::yaml);
    ec = glz::read<glz::opts{.format = glz::YAML, .null_terminated = false}>(config, content);
  }
  if (ec) {
    throw std::runtime_error(std::string("Config parse error: ") + glz::format_error(ec, content));
  }
}

// Internal: serialize a configuration as JSON / YAML. Throws std::invalid_argument on serialization errors.
template <class T>
std::string WriteConfig(const T& config, ConfigFormat format) {
  auto result =
      (format == ConfigFormat::json) ? glz::write_json(config) : glz::write<glz::opts{.format = glz::YAML}>(config);
  if (!result) {
    throw std::invalid_argument("Config serialization failed: " + glz::format_error(result.error()));
  }
  return std::move(result.value());
}

}  // namespace aeronet::detail
