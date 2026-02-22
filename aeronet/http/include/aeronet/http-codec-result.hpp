#pragma once

#include <cstdint>

#include "aeronet/http-status-code.hpp"

namespace aeronet::internal {

struct RequestDecompressionResult {
  http::StatusCode status{http::StatusCodeOK};
  const char* message = nullptr;
};

enum class CompressResponseResult : std::uint8_t {
  Uncompressed,     // response was not compressed (either because encoding not supported or config thresholds not met)
  Compressed,       // response was compressed and modified in-place
  ExceedsMaxRatio,  // response was compressed but did not meet the compression ratio requirement in config - response
                    // is left unmodified
  Error             // compression was attempted but failed (e.g. encoder error)
};

}  // namespace aeronet::internal