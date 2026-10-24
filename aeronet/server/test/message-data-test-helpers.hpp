#pragma once

#include <cstddef>
#include <string>

#include "aeronet/http-message-data.hpp"
#include "aeronet/raw-chars.hpp"

namespace aeronet::test {

// A message of 'size' bytes 'ch', without payload.
inline HttpMessageData MakeMessageData(std::size_t size, char ch) {
  return HttpMessageData(RawChars(std::string(size, ch)));
}

}  // namespace aeronet::test
