#include "header-merge.hpp"

#include <cstddef>
#include <cstring>
#include <string_view>

#include "headers-view-map.hpp"
#include "mergeable-headers.hpp"
#include "raw-chars.hpp"

namespace aeronet::http {

// In-place variant used by request header parsing where headers live inside a connection buffer.
// 'bufferBase' must point to the beginning of the connection receive buffer (state.inBuffer.data()).
// 'currentLineStart' is the pointer to the start of the header line being parsed (the 'first' pointer in setHead).
// 'tmp' is a temporary RawChars used to stage moved data (same tmpBuffer passed into setHead).
bool AddOrMergeHeaderInPlace(HeadersViewMap& map, std::string_view name, std::string_view value, RawChars& tmp,
                             const char* bufferBase, const char* currentLineStart,
                             bool mergeAllowedForUnknownRequestHeaders) {
  auto [it, inserted] = map.emplace(name, value);
  if (inserted) {
    return true;
  }

  // We have a duplicated header. We will append the duplicated value encountered now to the first key value pair
  // inplace in memory. Plan: use the tmp buffer to copy duplicate value there. In the examples
  // below, \r\n have been replaced by [] for readability (and they keep their true size). * marks 'garbage' memory
  // (moved from).
  //
  // Step 1 - copy v2 to tmp
  // Step 2 - move size(v2) + 1 to the right and update all pointers of _headers after v1
  // Step 3 - copy size(v2) to firstValueLast + 1, and set ',' to firstValueLast
  //
  // Host: example.com[]H: v1[]User-Agent: FooBar[]H: v2[]Other: v1[][]
  // Host: example.com[]H: v1***[]User-Agent: FooBar[]v2[]Other: v1[][]
  // Host: example.com[]H: v1,v2[]User-Agent: FooBar[]v2[]Other: v1[][]

  const auto mergeSep = ReqHeaderValueSeparator(it->first, mergeAllowedForUnknownRequestHeaders);
  if (mergeSep == '\0') {
    // Merge is forbidden, reject 400 Bad Request
    return false;
  }

  if (it->second.empty() || mergeSep == 'O') {
    // we keep the last value in the map (either second value is empty, or mergeSep is 'O' for override)
    it->second = value;
    return true;
  }

  if (value.empty()) {
    // second value is empty - we do nothing and just move on to the next headers, the first value is sufficient.
    return true;
  }

  // Both non empty - we actually do the merge

  // Compute sizes
  static constexpr std::size_t szSep = sizeof(mergeSep);
  std::size_t szToMove = static_cast<std::size_t>(value.size()) + szSep;

  // Step 1 - use tmp as temp data to keep value
  tmp.assign(value);

  // Step 2 - move existing suffix to the right to make room for new data
  auto* firstValueFirst = const_cast<char*>(bufferBase) + (it->second.data() - bufferBase);
  auto* firstValueLast = firstValueFirst + it->second.size();

  std::memmove(firstValueLast + szToMove, firstValueLast, static_cast<std::size_t>(currentLineStart - firstValueLast));
  for (auto& [key, val] : map) {
    if (key.data() > firstValueLast) {
      key = std::string_view(key.begin() + szToMove, key.end() + szToMove);
      val = std::string_view(val.begin() + szToMove, val.end() + szToMove);
    }
  }

  // Step 3 - finally copy tmp into the gap and insert separator
  std::memcpy(firstValueLast + szSep, tmp.data(), tmp.size());
  *firstValueLast = mergeSep;

  it->second = std::string_view(firstValueFirst, firstValueLast + szToMove);
  return true;
}

}  // namespace aeronet::http
