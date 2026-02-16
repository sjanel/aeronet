#pragma once

#include <string_view>

#include "aeronet/raw-chars.hpp"
#include "aeronet/vector.hpp"

namespace aeronet::internal {

struct SharedBuffers {
  // To avoid unbounded memory growth
  void shrink_to_fit() {
    buf.shrink_to_fit();
    decompressedBody.shrink_to_fit();
    trailers.shrink_to_fit();
    sv.shrink_to_fit();
  }

  RawChars buf;                 // can be used for any kind of temporary buffer
  RawChars decompressedBody;    // shared body buffer for non-async request decompression
  RawChars32 trailers;          // scratch buffer to preserve request trailers during decompression
  vector<std::string_view> sv;  // scratch vector for chunked decoding
};

}  // namespace aeronet::internal