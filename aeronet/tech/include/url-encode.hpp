#pragma once

#include <algorithm>
#include <span>

#include "char-hexadecimal-converter.hpp"
#include "string.hpp"

namespace aeronet {

/// This function converts the given input string to a URL encoded string.
/// All input characters 'ch' for which isNotEncodedFunc(ch) is false are converted in upper case hexadecimal.
/// (%NN where NN is a two-digit hexadecimal number).
template <class IsNotEncodedFunc>
string URLEncode(std::span<const char> data, IsNotEncodedFunc isNotEncodedFunc) {
  const auto nbNotEncodedChars = std::ranges::count_if(data, isNotEncodedFunc);
  const auto nbEncodedChars = data.size() - nbNotEncodedChars;

  string ret(nbNotEncodedChars + (3U * nbEncodedChars), '\0');

  char* outCharIt = ret.data();
  for (char ch : data) {
    if (isNotEncodedFunc(ch)) {
      *outCharIt++ = ch;
    } else {
      *outCharIt++ = '%';
      outCharIt = to_upper_hex(ch, outCharIt);
    }
  }
  return ret;
}

// const char * argument is deleted because it would construct into a span including the unwanted null
// terminating character. Use span directly, or string / string_view instead.
template <class IsNotEncodedFunc>
string URLEncode(const char*, IsNotEncodedFunc) = delete;

}  // namespace aeronet
