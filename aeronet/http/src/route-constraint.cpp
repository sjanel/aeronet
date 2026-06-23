#include "aeronet/route-constraint.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <regex>
#include <string_view>
#include <utility>

#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/object-array-pool.hpp"
#include "aeronet/vector.hpp"

namespace aeronet {

[[nodiscard]] bool RouteConstraint::CharClass::contains(char ch) const noexcept {
  return negated !=
         std::ranges::any_of(ranges, [ch](const CharRange& range) { return ch >= range.lo && ch <= range.hi; });
}

[[nodiscard]] bool RouteConstraint::FastPattern::matches(std::string_view value, vector<uint32_t>& usedPerAtom) const {
  const auto numAtoms = static_cast<uint32_t>(atoms.size());

  usedPerAtom.resize(numAtoms);

  uint32_t atomIdx = 0;
  std::size_t valuePos = 0;

  while (atomIdx < numAtoms) {
    const Atom& atom = atoms[atomIdx];
    uint32_t count = 0;

    while (count < atom.maxRep && valuePos + count < value.size() && atom.charClass.contains(value[valuePos + count])) {
      ++count;
    }

    if (count >= atom.minRep) {
      usedPerAtom[atomIdx] = count;
      valuePos += count;
      ++atomIdx;
    } else {
      goto backtrack;
    }
  }

  if (valuePos == value.size()) {
    return true;
  }

backtrack:
  while (atomIdx > 0) {
    --atomIdx;
    valuePos -= usedPerAtom[atomIdx];

    if (usedPerAtom[atomIdx] > atoms[atomIdx].minRep) {
      --usedPerAtom[atomIdx];
      valuePos += usedPerAtom[atomIdx];
      ++atomIdx;

      while (atomIdx < numAtoms) {
        const Atom& atom = atoms[atomIdx];
        uint32_t count = 0;

        while (count < atom.maxRep && valuePos + count < value.size() &&
               atom.charClass.contains(value[valuePos + count])) {
          ++count;
        }

        if (count >= atom.minRep) {
          usedPerAtom[atomIdx] = count;
          valuePos += count;
          ++atomIdx;
        } else {
          goto backtrack;
        }
      }

      if (valuePos == value.size()) {
        return true;
      }
      goto backtrack;
    }
  }

  return false;
}

RouteConstraint::CharClass RouteConstraint::ParseCharClass(std::string_view pattern, std::size_t& pos) {
  CharClass cc;

  if (pos < pattern.size() && pattern[pos] == '^') {
    cc.negated = true;
    ++pos;
  }

  while (pos < pattern.size() && pattern[pos] != ']') {
    char ch = pattern[pos];

    // Handle escape within char class
    if (ch == '\\' && pos + 1 < pattern.size()) {
      ++pos;
      ch = pattern[pos];
      // Shorthand inside []
      if (ch == 'd') {
        cc.ranges.emplace_back('0', '9');
        ++pos;
        continue;
      }
      if (ch == 'w') {
        cc.ranges.emplace_back('a', 'z');
        cc.ranges.emplace_back('A', 'Z');
        cc.ranges.emplace_back('0', '9');
        cc.ranges.emplace_back('_', '_');
        ++pos;
        continue;
      }
      if (ch == 's') {
        cc.ranges.emplace_back(' ', ' ');
        cc.ranges.emplace_back('\t', '\t');
        cc.ranges.emplace_back('\n', '\n');
        cc.ranges.emplace_back('\r', '\r');
        ++pos;
        continue;
      }
      // Literal escaped char
    }

    // Check for range: a-z
    if (pos + 2 < pattern.size() && pattern[pos + 1] == '-' && pattern[pos + 2] != ']') {
      char hi = pattern[pos + 2];
      // Handle escaped end of range
      if (hi == '\\' && pos + 3 < pattern.size()) {
        hi = pattern[pos + 3];
        cc.ranges.emplace_back(ch, hi);
        pos += 4;
      } else {
        cc.ranges.emplace_back(ch, hi);
        pos += 3;
      }
    } else {
      cc.ranges.emplace_back(ch, ch);
      ++pos;
    }
  }

  if (pos >= pattern.size()) {
    throw std::regex_error(std::regex_constants::error_brack);
  }
  assert(pattern[pos] == ']');
  ++pos;  // consume ']'

  return cc;
}

namespace {
std::pair<uint32_t, uint32_t> ParseQuantifier(std::string_view pattern, std::size_t& pos) {
  if (pos >= pattern.size()) {
    return {1, 1};
  }

  switch (pattern[pos]) {
    case '+':
      ++pos;
      return {1, std::numeric_limits<uint32_t>::max()};
    case '*':
      ++pos;
      return {0, std::numeric_limits<uint32_t>::max()};
    case '?':
      ++pos;
      return {0, 1};
    case '{': {
      ++pos;
      // Parse {n} or {n,m} or {n,}
      uint32_t minVal = 0;
      while (pos < pattern.size() && pattern[pos] >= '0' && pattern[pos] <= '9') {
        minVal = (minVal * 10) + static_cast<uint32_t>(pattern[pos] - '0');
        ++pos;
      }
      if (pos < pattern.size() && pattern[pos] == '}') {
        ++pos;
        return {minVal, minVal};
      }
      if (pos < pattern.size() && pattern[pos] == ',') {
        ++pos;
        if (pos < pattern.size() && pattern[pos] == '}') {
          ++pos;
          return {minVal, std::numeric_limits<uint32_t>::max()};
        }
        uint32_t maxVal = 0;
        while (pos < pattern.size() && pattern[pos] >= '0' && pattern[pos] <= '9') {
          maxVal = (maxVal * 10) + static_cast<uint32_t>(pattern[pos] - '0');
          ++pos;
        }
        if (pos < pattern.size() && pattern[pos] == '}') {
          ++pos;
          return {minVal, maxVal};
        }
      }
      // Malformed quantifier
      return {0, 0};
    }
    default:
      return {1, 1};
  }
}
}  // namespace

bool RouteConstraint::TryCompileFast(std::string_view pattern, FastPattern& fast) {
  fast.atoms.clear();
  std::size_t pos = 0;

  while (pos < pattern.size()) {
    Atom atom;

    switch (pattern[pos]) {
      case '[':
        ++pos;
        atom.charClass = ParseCharClass(pattern, pos);
        break;
      case '.':
        // Matches any character except \n and \r (standard ECMAScript dot behavior)
        ++pos;
        atom.charClass.ranges.emplace_back('\n', '\n');
        atom.charClass.ranges.emplace_back('\r', '\r');
        atom.charClass.negated = true;
        break;
      case '\\':
        // Shorthand character classes
        if (pos + 1 >= pattern.size()) {
          return false;  // trailing backslash - fall back
        }
        ++pos;
        switch (pattern[pos]) {
          case 'd':
            atom.charClass.ranges.emplace_back('0', '9');
            break;
          case 'D':
            atom.charClass.ranges.emplace_back('0', '9');
            atom.charClass.negated = true;
            break;
          case 'w':
            atom.charClass.ranges.emplace_back('a', 'z');
            atom.charClass.ranges.emplace_back('A', 'Z');
            atom.charClass.ranges.emplace_back('0', '9');
            atom.charClass.ranges.emplace_back('_', '_');
            break;
          case 'W':
            atom.charClass.ranges.emplace_back('a', 'z');
            atom.charClass.ranges.emplace_back('A', 'Z');
            atom.charClass.ranges.emplace_back('0', '9');
            atom.charClass.ranges.emplace_back('_', '_');
            atom.charClass.negated = true;
            break;
          case 's':
            atom.charClass.ranges.emplace_back(' ', ' ');
            atom.charClass.ranges.emplace_back('\t', '\t');
            atom.charClass.ranges.emplace_back('\n', '\n');
            atom.charClass.ranges.emplace_back('\r', '\r');
            break;
          case 'S':
            atom.charClass.ranges.emplace_back(' ', ' ');
            atom.charClass.ranges.emplace_back('\t', '\t');
            atom.charClass.ranges.emplace_back('\n', '\n');
            atom.charClass.ranges.emplace_back('\r', '\r');
            atom.charClass.negated = true;
            break;
          default:
            // Literal escaped char (e.g. \. \\ \/ etc.)
            atom.charClass.ranges.emplace_back(pattern[pos], pattern[pos]);
            break;
        }
        ++pos;
        break;
      case '(':
        [[fallthrough]];
      case '|':
        [[fallthrough]];
      case ')':
        // These indicate complex patterns - fall back to std::regex
        return false;
      case '^':
        [[fallthrough]];
      case '$':
        // Anchors - fall back to std::regex (the fast matcher always anchors implicitly)
        return false;
      default:
        // Literal character
        atom.charClass.ranges.emplace_back(pattern[pos], pattern[pos]);
        ++pos;
        break;
    }

    // Parse quantifier
    auto [minRep, maxRep] = ParseQuantifier(pattern, pos);
    assert(pos > 0);
    if (minRep == 0 && maxRep == 0 && pattern[pos - 1] != '}') {
      return false;
    }
    atom.minRep = minRep;
    atom.maxRep = maxRep;

    fast.atoms.emplace_back(std::move(atom));
  }

  return true;
}

struct RouteConstraint::RegexImpl {
  std::regex regex;
};

RouteConstraint::RouteConstraint() noexcept = default;

RouteConstraint::RouteConstraint(std::string_view pattern) : _patternStr(pattern) { initializeCompiledState(pattern); }

RouteConstraint::RouteConstraint(std::string_view pattern, ObjectArrayPool<char>& charStorage) {
  initializeCompiledState(pattern);

  if (pattern.empty()) {
    return;
  }

  char* pDes = charStorage.allocateAndDefaultConstruct(pattern.size());
  Copy(pattern, pDes);
  _patternStr = {pDes, pattern.size()};
}

void RouteConstraint::initializeCompiledState(std::string_view pattern) {
  if (pattern.empty()) {
    return;
  }

  if (TryCompileFast(pattern, _fastPattern)) {
    _kind = Kind::Fast;
    return;
  }

  _kind = Kind::Regex;
  _regexImpl = std::make_unique<RegexImpl>(
      std::regex(pattern.data(), pattern.size(), std::regex::optimize | std::regex::ECMAScript));
}

RouteConstraint::RouteConstraint(RouteConstraint&&) noexcept = default;
RouteConstraint& RouteConstraint::operator=(RouteConstraint&&) noexcept = default;
RouteConstraint::~RouteConstraint() = default;

bool RouteConstraint::matches(std::string_view value, vector<uint32_t>& usedPerAtom) const {
  switch (_kind) {
    case Kind::Fast:
      return _fastPattern.matches(value, usedPerAtom);
    case Kind::Regex:
      assert(_regexImpl);
      return std::regex_match(value.begin(), value.end(), _regexImpl->regex);
    default:
      assert(_kind == Kind::Empty);
      return true;
  }
}

}  // namespace aeronet
