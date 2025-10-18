#pragma once

#include <cstdint>
#include <string>

namespace aeronet {

struct BuiltinProbesConfig {
  void validate() const;

  enum class ContentType : std::uint8_t { TextPlainUtf8 /*, TODO: Json */ };

  bool enabled{false};
  ContentType contentType{ContentType::TextPlainUtf8};
  std::string livenessPath{"/livez"};
  std::string readinessPath{"/readyz"};
  std::string startupPath{"/startupz"};
};

}  // namespace aeronet