#pragma once

#include "raw-chars.hpp"

namespace aeronet {

// In-place variant: decodes within the provided string buffer, compacting percent-encoded
// sequences and (optionally) translating '+' to space if plusAsSpace is true. Returns false on
// invalid encoding (truncated % or non-hex digits) leaving the string in an unspecified partially
// modified state (caller can decide to discard it). On success the string length is reduced to
// the decoded size.
bool URLDecodeInPlace(RawChars &str, bool plusAsSpace = false);

}  // namespace aeronet
