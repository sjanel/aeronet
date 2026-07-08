#pragma once

#include <charconv>
#include <concepts>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace aeronet {

// Logging helpers for StringToIntegral error paths. They are defined out-of-line in stringconv.cpp so
// that the (heavy) logging dependency stays out of this very widely-included header.
// Logs (critical) a full decode failure of 'src' into an integral.
void LogStringToIntegralFailure(std::string_view src);
// Logs (error) a partial decode: only 'decodedCount' chars of 'src' were consumed, yielding 'value'.
void LogStringToIntegralPartialDecode(std::ptrdiff_t decodedCount, std::string_view src, std::string_view value);

template <std::integral Integral>
Integral StringToIntegral(const char* begPtr, std::size_t len) {
  // No need to value initialize ret, std::from_chars will set it in case no error is returned
  // And in case of error, exception is thrown instead
  Integral ret;

  const char* endPtr = begPtr + len;
  const auto [ptr, errc] = std::from_chars(begPtr, endPtr, ret);

  if (errc != std::errc()) {
    LogStringToIntegralFailure(std::string_view(begPtr, len));
    throw std::invalid_argument("StringToIntegral conversion failed");
  }

  if (ptr != endPtr) {
    char bufRet[std::numeric_limits<decltype(ret)>::digits10 + 2];
    const char* pEndBuf = std::to_chars(bufRet, bufRet + sizeof(bufRet), ret).ptr;
    std::string_view retStr(bufRet, pEndBuf);
    LogStringToIntegralPartialDecode(ptr - begPtr, std::string_view(begPtr, len), retStr);
  }
  return ret;
}

template <std::integral Integral>
Integral StringToIntegral(std::string_view str) {
  return StringToIntegral<Integral>(str.data(), str.size());
}

}  // namespace aeronet
