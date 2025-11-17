#pragma once

#include <cstdint>
#include <string_view>

#include "aeronet/static-concatenated-strings.hpp"

namespace aeronet {

class BuiltinProbesConfig {
 public:
  void validate() const;

  [[nodiscard]] std::string_view livenessPath() const noexcept { return _paths[0]; }

  [[nodiscard]] std::string_view readinessPath() const noexcept { return _paths[1]; }

  [[nodiscard]] std::string_view startupPath() const noexcept { return _paths[2]; }

  BuiltinProbesConfig& withLivenessPath(std::string_view path) {
    _paths.set(0, path);
    return *this;
  }

  BuiltinProbesConfig& withReadinessPath(std::string_view path) {
    _paths.set(1, path);
    return *this;
  }

  BuiltinProbesConfig& withStartupPath(std::string_view path) {
    _paths.set(2, path);
    return *this;
  }

  // We may add more content types in the future.
  enum class ContentType : std::uint8_t { TextPlainUtf8 };

  bool enabled{false};

  ContentType contentType{ContentType::TextPlainUtf8};

 private:
  using Paths = StaticConcatenatedStrings<3, uint32_t>;

  Paths _paths{"/livez", "/readyz", "/startupz"};
};

}  // namespace aeronet