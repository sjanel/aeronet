#pragma once

#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include "raw-chars.hpp"

namespace aeronet {

/// Parses a string representation of a number of bytes.
/// string should contain an integral number (decimal not supported) possibly followed by one of these units:
///  - T, G, M, k for multiples of 1000
///  - Ti, Gi, Mi, Ki for multiples of 1024
/// Note: it is a simplified version of the syntax used by Kubernetes:
/// https://kubernetes.io/docs/concepts/configuration/manage-resources-containers/
int64_t ParseNumberOfBytes(std::string_view sizeStr);

/// Writes to 'buf' the string representation of the number of bytes.
/// If given buffer is too small, it will throw an exception.
/// Returns a span of the buffer containing the string representation.
std::span<char> BytesToBuffer(int64_t numberOfBytes, std::span<char> buf,
                              int nbSignificantUnits = std::numeric_limits<int>::max());

// Returns the length of the string representation of the number of bytes.
int64_t BytesToStrLen(int64_t numberOfBytes, int nbSignificantUnits = std::numeric_limits<int>::max());

// Same than BytesToBuffer, but returns a RawChars instead.
RawChars BytesToStr(int64_t numberOfBytes, int nbSignificantUnits = std::numeric_limits<int>::max());

}  // namespace aeronet
