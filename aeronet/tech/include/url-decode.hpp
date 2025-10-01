#pragma once

namespace aeronet::url {

// Decodes within the provided string buffer, compacting percent-encoded
// sequences and (optionally) translating '+' to space if plusAsSpace is true. Returns nullptr on
// invalid encoding (truncated % or non-hex digits) leaving the string in an unspecified partially
// modified state (caller can decide to discard it).
// Returns a pointer to the new logical end of the 'str' decoded sequence.
// plusAsSpace should be used only for query string values, not for paths.
char* DecodeInPlace(char* first, char* last, char plusAs = '+', bool strictInvalid = true);

inline constexpr char kNewKeyValueSep = 0x1F;
inline constexpr char kNewPairSep = '\0';

// Decodes all key/value pairs inside queryParams in-place as best effort.
// Do not decode percent encoded chars if it would result in an error.
// Returns pointer to the new logical end of queryParams.
// We use the application/x-www-form-urlencoded format here: + are converted into spaces for values
char* DecodeQueryParamsInPlace(char* first, char* last);

}  // namespace aeronet::url
