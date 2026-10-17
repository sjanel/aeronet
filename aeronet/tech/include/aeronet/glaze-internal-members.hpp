#pragma once

#include <glaze/glaze.hpp>
#include <string_view>
#include <type_traits>

namespace aeronet::glz_detail {

// Configuration objects are aggregates, so that glaze reflects their members without listing them: a new field is
// serialized automatically. Their internal storage (packed strings, validated lists...) is held by public members
// prefixed with '_', which are not configuration keys: the glz::meta of such an object skips them, and exposes
// their content through the accessors in its 'modify' list instead.
constexpr bool IsInternalMember(std::string_view key) noexcept { return key.starts_with('_'); }

// glz::meta base skipping the internal members (see IsInternalMember) of a reflected configuration object.
struct SkipInternalMembers {
  static constexpr bool skip(std::string_view key, const glz::meta_context& /*context*/) noexcept {
    return IsInternalMember(key);
  }
};

// Number of entries of a glz::object(key, value, ...) whose entries all have an explicit key.
template <class Object>
constexpr auto NbObjectEntries = glz::tuple_size_v<std::remove_cvref_t<decltype(Object::value)>> / 2U;

}  // namespace aeronet::glz_detail
