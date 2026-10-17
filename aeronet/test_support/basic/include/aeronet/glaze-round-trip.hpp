#pragma once

#include <cstdint>
#include <glaze/glaze.hpp>
#include <limits>
#include <string>

namespace aeronet::test {

// Change every boolean and number of a generic JSON value, recursively. Strings are kept as is: they may be enum
// names, durations or versions, which only accept specific values.
inline void MutateScalars(glz::generic_u64& value) {
  if (value.is_object()) {
    for (auto& [key, child] : value.get_object()) {
      MutateScalars(child);
    }
  } else if (value.is_array()) {
    for (auto& child : value.get_array()) {
      MutateScalars(child);
    }
  } else if (value.is_boolean()) {
    value = !value.get<bool>();
  } else if (value.holds<std::uint64_t>()) {
    const auto number = value.get<std::uint64_t>();
    value = number == std::numeric_limits<std::uint64_t>::max() ? number - 1U : number + 1U;
  } else if (value.holds<std::int64_t>()) {
    value = value.get<std::int64_t>() + 1;
  } else if (value.holds<double>()) {
    value = value.get<double>() + 1.0;
  }
}

struct ScalarsRoundTrip {
  // JSON of a default constructed T, with every boolean and number changed (see MutateScalars).
  std::string expected;
  // 'expected', read into a default constructed T, then written back (or the read error).
  std::string actual;
};

// Checks that each boolean and number written for a T is read back into the same field, whatever the field: serialize
// a default T, change every serialized scalar, read the result into a T and serialize it again. 'actual' equals
// 'expected' when every scalar field round-trips, so that a new field is checked without updating any test.
template <class T>
ScalarsRoundTrip MutateScalarsRoundTrip() {
  ScalarsRoundTrip result;
  glz::generic_u64 generic;
  if (const auto ec = glz::read_json(generic, glz::write_json(T{}).value_or(std::string{})); ec) {
    result.actual = "cannot parse the JSON of a default object: " + glz::format_error(ec);
    return result;
  }
  MutateScalars(generic);
  result.expected = glz::write_json(generic).value_or(std::string{});

  T object{};
  if (const auto ec = glz::read_json(object, result.expected); ec) {
    result.actual = "cannot read the mutated JSON: " + glz::format_error(ec, result.expected);
    return result;
  }
  // Written through glz::generic_u64 as well, so that both strings use the same number formatting.
  glz::generic_u64 roundTripped;
  if (const auto ec = glz::read_json(roundTripped, glz::write_json(object).value_or(std::string{})); ec) {
    result.actual = "cannot parse the round-tripped JSON: " + glz::format_error(ec);
    return result;
  }
  result.actual = glz::write_json(roundTripped).value_or(std::string{});
  return result;
}

}  // namespace aeronet::test
