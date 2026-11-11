#include "aeronet/header-merge.hpp"

#include <cstddef>
#include <cstring>
#include <string_view>

#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/mergeable-headers.hpp"
#include "aeronet/raw-chars.hpp"
#include "aeronet/sv-to-sv-map.hpp"

namespace aeronet::http {

bool MergeHeaderInPlace(SvToSvMap& map, SvToSvMap::iterator it, std::string_view newValue, RawChars& tmp,
                        char* pBufferBase, const char* pCurrentLine, bool mergeAllowedForUnknownRequestHeaders) {
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
  const char mergeSep = ReqHeaderValueSeparator(it->first, mergeAllowedForUnknownRequestHeaders);
  if (mergeSep == '\0') {
    // Merge is forbidden, reject 400 Bad Request
    return false;
  }

  if (it->second.empty()) {
    // the first value is empty - we keep the last value in the map
    it->second = newValue;
    return true;
  }

  if (newValue.empty()) {
    // second value is empty - we do nothing and just move on to the next headers, the first value is sufficient.
    return true;
  }

  // Both non empty - we actually do the merge

  // Compute sizes
  static constexpr std::size_t szSep = sizeof(mergeSep);  // 1
  const std::size_t szToMove = newValue.size() + szSep;

  // Step 1 - use tmp as temp data to keep value
  tmp.assign(newValue);

  // Step 2 - move existing suffix to the right to make room for new data
  char* pFirstValueBeg = pBufferBase + (it->second.data() - pBufferBase);
  char* pFirstValueEnd = pFirstValueBeg + it->second.size();

  std::memmove(pFirstValueEnd + szToMove, pFirstValueEnd, static_cast<std::size_t>(pCurrentLine - pFirstValueEnd));

  // Step 3 - finally insert separator and copy tmp into the gap
  *pFirstValueEnd = mergeSep;
  Copy(tmp, pFirstValueEnd + szSep);

  // Update all pointers in map that are after firstValueLast
  for (auto& [key, val] : map) {
    if (key.data() > pFirstValueEnd) {
      key = std::string_view(key.data() + szToMove, key.size());
    }
    if (val.data() > pFirstValueEnd) {
      val = std::string_view(val.data() + szToMove, val.size());
    }
  }

  it->second = std::string_view(pFirstValueBeg, pFirstValueEnd + szToMove);
  return true;
}

}  // namespace aeronet::http
