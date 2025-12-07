#include "aeronet/mime-mappings.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <string_view>

#include "aeronet/toupperlower.hpp"

namespace aeronet {

static_assert(std::ranges::is_sorted(kMIMEMappings, {}, &MIMEMapping::extension),
              "kMIMEMappings must be sorted by extension");

static_assert(std::size(kMIMEMappings) < std::numeric_limits<MIMETypeIdx>::max(),
              "kMIMEMappings size exceeds MIMETypeIdx capacity");

MIMETypeIdx DetermineMIMETypeIdx(std::string_view path) {
  const auto dotPos = path.rfind('.');

  static constexpr std::size_t kMaximumKnownExtensionSize =
      std::ranges::max_element(kMIMEMappings, [](const auto &lhs, const auto &rhs) {
        return lhs.extension.size() < rhs.extension.size();
      })->extension.size();

  if (dotPos != std::string_view::npos && (path.size() - dotPos - 1U) <= kMaximumKnownExtensionSize) {
    char extBuf[kMaximumKnownExtensionSize];
    const auto endIt =
        std::transform(path.begin() + dotPos + 1U, path.end(), extBuf, [](char ch) { return tolower(ch); });

    const std::string_view ext(extBuf, endIt);
    const auto it = std::ranges::lower_bound(kMIMEMappings, ext, {}, &MIMEMapping::extension);
    if (it != std::end(kMIMEMappings) && it->extension == ext) {
      return static_cast<MIMETypeIdx>(std::distance(std::begin(kMIMEMappings), it));
    }
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