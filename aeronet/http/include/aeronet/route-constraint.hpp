#pragma once

#include <amc/type_traits.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>

#include "aeronet/object-array-pool.hpp"
#include "aeronet/small-vector.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

// Compiled constraint for a route parameter.
// Supports two tiers: a fast custom character-class matcher for simple patterns
// (e.g. [0-9]+, [a-zA-Z_-]*, .{3,8}, \d+), and a std::regex fallback for complex
// patterns (alternation, groups, backreferences).
// A default-constructed RouteConstraint is "unconstrained" and always matches.
class RouteConstraint {
 public:
  RouteConstraint() noexcept;

  // Compile a constraint pattern. Throws std::regex_error when the pattern is invalid.
  // An empty pattern is treated as unconstrained.
  explicit RouteConstraint(std::string_view pattern);

  // Compile a constraint pattern and persist its source string into the provided char storage.
  // This is used when the incoming pattern view does not outlive the compiled constraint.
  RouteConstraint(std::string_view pattern, ObjectArrayPool<char>& charStorage);

  RouteConstraint(const RouteConstraint& other) = delete;
  RouteConstraint& operator=(const RouteConstraint& other) = delete;

  RouteConstraint(RouteConstraint&&) noexcept;
  RouteConstraint& operator=(RouteConstraint&&) noexcept;

  ~RouteConstraint();

  // Test whether a captured parameter value satisfies this constraint.
  // Returns true if unconstrained (no pattern).
  [[nodiscard]] bool matches(std::string_view value, vector<uint32_t>& usedPerAtom) const;

  // True if this constraint has no pattern (always matches).
  [[nodiscard]] bool empty() const noexcept { return _kind == Kind::Empty; }

  // Access the original pattern string (empty if unconstrained).
  [[nodiscard]] std::string_view pattern() const noexcept { return _patternStr; }

 private:
  struct CharRange {
    char lo;
    char hi;
  };

  struct CharClass {
    [[nodiscard]] bool contains(char ch) const noexcept;

    SmallVector<CharRange, 4U> ranges;
    bool negated{false};

    using trivially_relocatable = amc::is_trivially_relocatable<decltype(ranges)>::type;
  };

  struct Atom {
    using trivially_relocatable = amc::is_trivially_relocatable<CharClass>::type;

    CharClass charClass;
    uint32_t minRep{1};
    uint32_t maxRep{1};
  };

  struct FastPattern {
    [[nodiscard]] bool matches(std::string_view value, vector<uint32_t>& usedPerAtom) const;

    vector<Atom> atoms;

    using trivially_relocatable = amc::is_trivially_relocatable<decltype(atoms)>::type;
  };

  struct RegexImpl;

  enum class Kind : uint8_t { Empty, Fast, Regex };

  static CharClass ParseCharClass(std::string_view pattern, std::size_t& pos);
  static bool TryCompileFast(std::string_view pattern, FastPattern& fast);
  void initializeCompiledState(std::string_view pattern);

  Kind _kind{Kind::Empty};
  std::string_view _patternStr;
  FastPattern _fastPattern;
  std::unique_ptr<RegexImpl> _regexImpl;

 public:
  using trivially_relocatable = std::bool_constant<amc::is_trivially_relocatable<FastPattern>::value>;
};

}  // namespace aeronet
