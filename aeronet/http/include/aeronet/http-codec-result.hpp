#pragma once

#include <cstdint>

#include "aeronet/http-status-code.hpp"

namespace aeronet::internal {

struct RequestDecompressionResult {
  http::StatusCode status{http::StatusCodeOK};
  const char* message = nullptr;
};

enum class CompressResponseResult : std::uint8_t {
  uncompressed,     // response was not compressed (either because encoding not supported or config thresholds not met)
  compressed,       // response was compressed and modified in-place
  exceedsMaxRatio,  // response was compressed but did not meet the compression ratio requirement in config - response
                    // is left unmodified
  error             // compression was attempted but failed (e.g. encoder error)
};

}  // namespace aeronet::internal