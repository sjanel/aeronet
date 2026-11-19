#include "uncaught-errors.hpp"

#include <cassert>
#include <cstddef>
#include <string_view>

#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/object-array-pool.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::internal {

void UncaughtErrors::add(std::string_view text) {
  if (_texts.size() == kMaxCount) {
    return;
  }

  static constexpr std::string_view kCutSuffix = "...";

  std::string_view kept = text;
  std::string_view suffix;
  if (text.size() > kMaxSize) {
    // Valid UTF-8, as QuickJS-ng converts strings: a sequence starts at most 3 bytes before a continuation byte.
    std::size_t size = kMaxSize;
    while ((static_cast<unsigned char>(text[size]) & 0xC0U) == 0x80U) {
      assert(size > 0);
      --size;
    }
    kept = text.substr(0, size);
    suffix = kCutSuffix;
  }

  const std::size_t size = kept.size() + suffix.size();
  // The pool keeps a text whose view could not be added until the page dies: only on memory exhaustion.
  char* pText = _chars.allocateAndDefaultConstruct(static_cast<ObjectArrayPool<char>::size_type>(size));
  [[maybe_unused]] const char* pEnd = Append(suffix, Append(kept, pText));
  assert(pEnd == pText + size);
  _texts.emplace_back(pText, size);
}

}  // namespace aeronet::internal
