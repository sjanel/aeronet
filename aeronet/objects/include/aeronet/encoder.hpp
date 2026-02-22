#pragma once

#include <cstddef>
#include <string_view>

#include "aeronet/encoder-result.hpp"

namespace aeronet {

class EncoderContext {
 public:
  virtual ~EncoderContext() = default;

  // Return the minimum capacity needed before a call to encodeChunk().
  [[nodiscard]] virtual std::size_t minEncodeChunkCapacity(std::size_t chunkSize) const = 0;

  // Returns the minimal buffer size needed to hold data produced by a single end() call.
  [[nodiscard]] virtual std::size_t endChunkSize() const = 0;

  // Streaming chunk encoder.
  // You should not call encodeChunk() again after having finished the stream.
  virtual EncoderResult encodeChunk(std::string_view data, std::size_t availableCapacity, char* buf) = 0;

  // Finalize the encoding stream, writing any remaining bytes into 'buf'.
  // May require multiple calls until it returns 0. Further calls after 0 are undefined.
  virtual EncoderResult end(std::size_t availableCapacity, char* buf) noexcept = 0;
};

}  // namespace aeronet