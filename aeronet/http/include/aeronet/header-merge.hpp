#pragma once

#include <string_view>

#include "aeronet/headers-view-map.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet::http {

// In-place variant used by request header parsing where headers live inside a connection buffer.
// 'bufferBase' must point to the beginning of the connection receive buffer (state.inBuffer.data()).
// 'currentLineStart' is the pointer to the start of the header line being parsed (the 'first' pointer in setHead).
// 'tmp' is a temporary RawChars used to stage moved data (same tmpBuffer passed into setHead).
bool AddOrMergeHeaderInPlace(HeadersViewMap& map, std::string_view name, std::string_view value, RawChars& tmp,
                             const char* bufferBase, const char* currentLineStart,
                             bool mergeAllowedForUnknownRequestHeaders);

}  // namespace aeronet::http