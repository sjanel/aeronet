#pragma once

#include <string_view>

namespace aeronet::internal {

// The JSON object or array literal assigned to 'variable' in the JavaScript source 'script' (see
// HtmlDocument::assignedJson): from its opening to its matching closing bracket, strings skipped, or an empty view if
// there is no such assignment, or if the first one is not closed.
[[nodiscard]] std::string_view FindAssignedJson(std::string_view script, std::string_view variable) noexcept;

}  // namespace aeronet::internal
