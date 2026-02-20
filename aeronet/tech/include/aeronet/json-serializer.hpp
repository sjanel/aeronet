#pragma once

#ifdef AERONET_ENABLE_GLAZE

#include <glaze/glaze.hpp>  // IWYU pragma: export
#include <string>

namespace aeronet {

/// Serialize a C++ object to JSON string using glaze.
/// Template parameter T must be a type that glaze can serialize.
/// Example usage:
///   struct Message { std::string text; };
///   template<> struct glz::meta<Message> { ... };
///   auto json_str = aeronet::SerializeToJson(msg);
template <typename T>
[[nodiscard]] inline std::string SerializeToJson(const T& obj) {
  return glz::write_json(obj).value_or(std::string{});
}

}  // namespace aeronet

#endif  // AERONET_ENABLE_GLAZE
