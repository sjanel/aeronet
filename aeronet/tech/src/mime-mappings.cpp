#include "aeronet/mime-mappings.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <string_view>

namespace aeronet {

static_assert(std::ranges::is_sorted(kMIMEMappings, {}, &MIMEMapping::extensionCode),
              "kMIMEMappings must be sorted by extensionCode");

static_assert(std::size(kMIMEMappings) < std::numeric_limits<MIMETypeIdx>::max(),
              "kMIMEMappings size exceeds MIMETypeIdx capacity");

MIMETypeIdx DetermineMIMETypeIdx(std::string_view path) {
  const auto dotPos = path.rfind('.');
  if (dotPos == std::string_view::npos) {
    return kUnknownMIMEMappingIdx;
  }
  const std::size_t extLen = path.size() - dotPos - 1U;
  if (extLen == 0 || extLen > MIMEExtensionCode::kMaxChars) {
    return kUnknownMIMEMappingIdx;
  }

  const MIMEExtensionCode query(path.substr(dotPos + 1U));
  const auto it = std::ranges::lower_bound(kMIMEMappings, query, {}, &MIMEMapping::extensionCode);
  if (it != std::end(kMIMEMappings) && it->extensionCode == query) {
    return static_cast<MIMETypeIdx>(std::distance(std::begin(kMIMEMappings), it));
  }
  return kUnknownMIMEMappingIdx;
}

std::string_view DetermineMIMETypeStr(std::string_view path) {
  const MIMETypeIdx idx = DetermineMIMETypeIdx(path);
  if (idx != kUnknownMIMEMappingIdx) {
    return kMIMEMappings[idx].mimeType;
  }
  return {};
}

}  // namespace aeronet