#pragma once

#include <string_view>

#include "aeronet/raw-chars.hpp"
#include "aeronet/sv-to-sv-map.hpp"

namespace aeronet::http {

// In-place variant used by request header parsing where headers live inside a connection buffer.
// 'pBufferBase' must point to the beginning of the connection receive buffer.
// 'pCurrentLine' is the pointer to the start of the header line being parsed (the 'first' pointer in setHead).
// 'tmp' is a temporary RawChars used to stage moved data (same tmpBuffer passed into setHead).
bool MergeHeaderInPlace(SvToSvMap& map, SvToSvMap::iterator it, std::string_view newValue, RawChars& tmp,
                        char* pBufferBase, const char* pCurrentLine, bool mergeAllowedForUnknownRequestHeaders);

}  // namespace aeronet::http