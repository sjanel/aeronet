#pragma once

#include <algorithm>
#include <span>
#include <string>

#include "char-hexadecimal-converter.hpp"

namespace aeronet {

/// This function converts the given input string to a URL encoded string.
/// All input characters 'ch' for which isNotEncodedFunc(ch) is false are converted in upper case hexadecimal.
/// (%NN where NN is a two-digit hexadecimal number).
template <class IsNotEncodedFunc>
std::string URLEncode(std::span<const char> data, IsNotEncodedFunc isNotEncodedFunc) {
  const std::size_t nbNotEncodedChars = static_cast<std::size_t>(std::ranges::count_if(data, isNotEncodedFunc));
  const std::size_t nbEncodedChars = data.size() - nbNotEncodedChars;

  std::string ret(nbNotEncodedChars + (3U * nbEncodedChars), '\0');

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
std::string URLEncode(const char*, IsNotEncodedFunc) = delete;

}  // namespace aeronet
