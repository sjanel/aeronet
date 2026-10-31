#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace aeronet::http {

using MethodBmp = uint16_t;

#ifdef AERONET_WINDOWS
// On Windows DELETE is defined as a macro in winnt.h... :/
#ifdef DELETE
#undef DELETE
#endif
#endif

// GET/POST dominate real traffic; PATCH/CONNECT/TRACE are rare. Ordering
// matters here because the HTTP/1 request line parser scans the method codes linearly.
enum class Method : MethodBmp {
  GET = 1U << 0U,
  POST = 1U << 1U,
  PUT = 1U << 2U,
  DELETE = 1U << 3U,
  HEAD = 1U << 4U,
  OPTIONS = 1U << 5U,
  PATCH = 1U << 6U,
  CONNECT = 1U << 7U,
  TRACE = 1U << 8U,
};

inline constexpr Method kMethodInvalid = static_cast<Method>(0);

inline constexpr std::string_view kMethodStrings[]{
    "GET", "POST", "PUT", "DELETE", "HEAD", "OPTIONS", "PATCH", "CONNECT", "TRACE",
};

using MethodIdx = uint8_t;

inline constexpr MethodIdx kNbMethods = std::size(kMethodStrings);

constexpr MethodBmp operator|(Method lhs, Method rhs) noexcept {
  return static_cast<MethodBmp>(static_cast<MethodBmp>(lhs) | static_cast<MethodBmp>(rhs));
}

constexpr MethodBmp operator|(MethodBmp lhs, Method rhs) noexcept {
  return static_cast<MethodBmp>(lhs | static_cast<MethodBmp>(rhs));
}

constexpr MethodBmp operator|(Method lhs, MethodBmp rhs) noexcept {
  return static_cast<MethodBmp>(static_cast<MethodBmp>(lhs) | rhs);
}

static_assert(kNbMethods <= sizeof(MethodBmp) * 8,
              "MethodBmp type too small to hold all methods; increase size or change type");

// Check if a method is allowed by mask.
constexpr bool IsMethodSet(MethodBmp mask, Method method) { return (mask & static_cast<MethodBmp>(method)) != 0U; }

// Methods whose repetition has the same observable effect as a single invocation (RFC 9110 §9.2.2), so a
// fully-sent request is safe to re-submit. POST and PATCH are not idempotent; CONNECT is neither safe nor
// idempotent. Used by the client retry policy to decide whether a post-send failure may be retried.
inline constexpr MethodBmp kIdempotentMethods =
    Method::GET | Method::PUT | Method::DELETE | Method::HEAD | Method::OPTIONS | Method::TRACE;

constexpr bool IsIdempotent(Method method) noexcept { return IsMethodSet(kIdempotentMethods, method); }

constexpr bool IsMethodIdxSet(MethodBmp mask, MethodIdx methodIdx) { return (mask & (1U << methodIdx)) != 0U; }

constexpr MethodIdx MethodToIdx(Method method) {
  return static_cast<MethodIdx>(std::countr_zero(static_cast<MethodBmp>(method)));
}

constexpr Method MethodFromIdx(MethodIdx methodIdx) { return static_cast<http::Method>(1U << methodIdx); }
constexpr MethodBmp MethodBmpFromIdx(MethodIdx methodIdx) { return static_cast<http::MethodBmp>(1U << methodIdx); }

constexpr std::string_view MethodIdxToStr(MethodIdx methodIdx) {
  assert(methodIdx < kNbMethods);
  return kMethodStrings[methodIdx];
}

constexpr std::string_view MethodToStr(Method method) { return MethodIdxToStr(MethodToIdx(method)); }

// Attempt to parse a HTTP method.
// RFC 9110 §9.1: The method token is case-sensitive, so "get" is not "GET".
// Returns kMethodInvalid if the method is not recognized.
constexpr Method ParseMethodStr(std::string_view str) noexcept {
  // Compilers fully unroll this loop over the constexpr table into a dispatch on the length followed by one or two
  // inlined integer comparisons - measured as fast as (or faster than) a hand-written switch with clang and gcc.
  for (MethodIdx methodIdx = 0; methodIdx < kNbMethods; ++methodIdx) {
    if (str == kMethodStrings[methodIdx]) {
      return MethodFromIdx(methodIdx);
    }
  }
  return kMethodInvalid;
}

inline constexpr std::size_t kAllMethodsStrLen = [] {
  std::size_t len = 0;
  for (MethodIdx methodIdx = 0; methodIdx < kNbMethods; ++methodIdx) {
    len += MethodIdxToStr(methodIdx).size();
  }
  return len;
}();

// Length of the list of all methods written by AppendMethodList.
inline constexpr std::size_t kMethodListMaxLen = kAllMethodsStrLen + (static_cast<std::size_t>(kNbMethods - 1U) * 2U);

// Writes the methods set in 'methodBmp' at 'pOut', in index order and separated by ", " (the syntax of the Allow and
// Access-Control-Allow-Methods values), and returns the end of the list. 'pOut' must have room for kMethodListMaxLen
// chars. Nothing is written for an empty bitmap.
constexpr char* AppendMethodList(MethodBmp methodBmp, char* pOut) {
  const char* pBeg = pOut;
  for (MethodIdx methodIdx = 0; methodIdx < kNbMethods; ++methodIdx) {
    if (!IsMethodIdxSet(methodBmp, methodIdx)) {
      continue;
    }
    if (pOut != pBeg) {
      *pOut++ = ',';
      *pOut++ = ' ';
    }
    const std::string_view methodStr = MethodIdxToStr(methodIdx);
    pOut += methodStr.copy(pOut, methodStr.size());
  }
  return pOut;
}

}  // namespace aeronet::http