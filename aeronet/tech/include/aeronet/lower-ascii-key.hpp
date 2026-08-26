#pragma once

#include <cassert>
#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace aeronet {

// A std::string_view guaranteed to contain no ASCII upper-case letter ('A'-'Z') -- guaranteed at compile time when
// built from a string literal, or via assert() only (debug builds) when built from a dynamic std::string_view. Used
// wherever an already-normalized-to-lower-case key is required, e.g. all HttpMessage and HttpRequestView methods taking
// one header or trailer name. Header and trailer names are normalized to lower-case at protocol/configuration ingress,
// but the type itself carries no HTTP-specific semantics -- it can be reused anywhere a similar "pre-lowered key"
// contract is needed.
//
// Two construction paths, intentionally asymmetric:
//
//  1) From a string literal / any fixed-size `constexpr char[N]` (implicit, consteval):
//       req.headerValue("content-type");   // OK, validated at compile time
//       req.headerValue("Content-Type");   // does not compile
//     PRECONDITION (enforced at COMPILE TIME): no character in [A-Z]. Violation => hard compile error at the call site
//     (or at the point of definition, if used to initialize a namespace-scope constant -- see
//     aeronet::http::ContentType and siblings for the intended pattern).
//
//  2) From a dynamic std::string_view (explicit, noexcept):
//       LowerAsciiKey{myRuntimeSv}
//     PRECONDITION (checked with assert() ONLY, i.e. ONLY in debug builds -- NDEBUG undefined): no character in
//     [A-Z]. In Release builds (NDEBUG defined) this is NOT checked at all: passing a key that isn't already
//     lower-case is undefined behavior on the caller's part: a lookup may miss and an insertion may violate normalized
//     storage. It is the caller's responsibility to lower-case dynamic keys beforehand (see tolower-str.hpp) before
//     wrapping them here. The constructor is deliberately `explicit`: this makes every such runtime-only-checked call
//     site visually distinct from a compile-time-validated literal, which is the whole point of this type existing
//     rather than passing raw std::string_view around.
//
// Implicitly convertible back to std::string_view (see operator std::string_view() below) so that, once constructed, it
// behaves exactly like a plain string_view everywhere downstream (map lookups, comparisons, etc.) -- the friction is
// only ever on the way in, never on the way out.
class LowerAsciiKey {
 public:
  // Default-constructed LowerAsciiKey is empty (and obviously valid).
  constexpr LowerAsciiKey() noexcept = default;

  // Implicit on purpose: lets call sites pass literals directly, e.g. req.headerValue("content-type").
  // See class-level doc, construction path 1, for the compile-time precondition this enforces.
  template <std::size_t N>
  consteval LowerAsciiKey(const char (&str)[N]) : _sv(str, N - 1) {  // NOLINT(*-explicit-constructor)
    if (ContainsUpperAscii(_sv)) {
      throw std::invalid_argument("LowerAsciiKey: value must not contain ASCII upper-case letters");
    }
  }

  // See class-level doc, construction path 2. Explicit so that "this key is only checked at runtime, and only
  // in debug builds" is always visible at the call site -- never accidentally implicit.
  explicit constexpr LowerAsciiKey(std::string_view sv) noexcept : _sv(sv) {
    assert(!ContainsUpperAscii(_sv) && "LowerAsciiKey: value must be ASCII lower-case already (tolower() it first)");
  }

  // Returns the underlying string_view.
  [[nodiscard]] constexpr std::string_view get() const noexcept { return _sv; }

  // Returns true if the underlying string_view is empty.
  [[nodiscard]] constexpr bool empty() const noexcept { return _sv.empty(); }

  // Returns the underlying string_view's data pointer.
  [[nodiscard]] constexpr const char* data() const noexcept { return _sv.data(); }

  // Returns the underlying string_view's size.
  [[nodiscard]] constexpr std::size_t size() const noexcept { return _sv.size(); }

  // Defaulted equality operator: compares the underlying string_view.
  constexpr bool operator==(const LowerAsciiKey&) const noexcept = default;

  // Transparent conversion so a LowerAsciiKey can be used directly wherever a std::string_view is expected
  // (heterogeneous map lookups, comparisons, formatting, etc.) without an explicit .get() at every use.
  constexpr operator std::string_view() const noexcept { return _sv; }  // NOLINT(*-explicit-constructor)

 private:
  static constexpr bool ContainsUpperAscii(std::string_view sv) noexcept {
    // Use raw loop to avoid <algorithm> include
    // NOLINTNEXTLINE(readability-use-anyofallof)
    for (char ch : sv) {
      if (ch >= 'A' && ch <= 'Z') {
        return true;
      }
    }
    return false;
  }

  std::string_view _sv;
};

}  // namespace aeronet
