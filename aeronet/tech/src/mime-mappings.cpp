#include "aeronet/mime-mappings.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <string_view>

#include "aeronet/tolower-str.hpp"

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

  const std::size_t extLen = path.size() - dotPos - 1U;
  if (dotPos != std::string_view::npos && extLen <= kMaximumKnownExtensionSize) {
    char extBuf[kMaximumKnownExtensionSize];

    tolower_n(path.data() + dotPos + 1U, extLen, extBuf);

    const std::string_view ext(extBuf, extLen);
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