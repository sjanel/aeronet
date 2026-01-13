#pragma once

#include <string_view>

namespace aeronet {

class EncoderContext {
 public:
  virtual ~EncoderContext() = default;

  // Streaming chunk encoder.
  // Provide an empty chunk of 'data' if and only if finishing the stream.
  // You should not call encodeChunk() again after having finished the stream.
  virtual std::string_view encodeChunk(std::string_view data) = 0;
};

}  // namespace aeronet