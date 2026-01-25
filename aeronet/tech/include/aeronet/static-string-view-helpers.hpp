#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "aeronet/nchars.hpp"

namespace aeronet {

/// Concatenates variadic template std::string_view arguments at compile time and defines a std::string_view pointing on
/// a static storage. The storage is guaranteed to be null terminated (but not itself included in the returned value)
/// Adapted from
/// https://stackoverflow.com/questions/38955940/how-to-concatenate-static-strings-at-compile-time/62823211#62823211
template <std::string_view const&... Strs>
class JoinStringView {
 private:
  // Join all strings into a single std::array of chars
  static constexpr auto impl() noexcept {
    constexpr std::string_view::size_type len = (Strs.size() + ... + 0);
    std::array<char, len + 1U> charsArray;  // +1 for null terminated char
    if constexpr (len > 0) {
      auto append = [it = charsArray.begin()](auto const& chars) mutable {
        // Voluntarily not using std::ranges::copy to avoid including <algorithm> in a header file
        for (auto ch : chars) {
          *it = ch;
          ++it;
        }
      };
      (append(Strs), ...);
    }
    charsArray.back() = '\0';
    return charsArray;
  }

  // Give the joined string static storage
  static constexpr auto arr = impl();

 public:
  // View as a std::string_view
  static constexpr std::string_view value{arr.data(), arr.size() - 1};

  // c_str version (null-terminated)
  static constexpr const char* const c_str = arr.data();
};

// Helper to get the value out
template <std::string_view const&... Strs>
inline constexpr auto JoinStringView_v = JoinStringView<Strs...>::value;

/// Same as JoinStringView but with a char separator between each string_view
template <std::string_view const& Sep, std::string_view const&... Strs>
class JoinStringViewWithSep {
 private:
  // Join all strings into a single std::array of chars
  static constexpr auto impl() noexcept {
    static constexpr std::string_view::size_type len = (Strs.size() + ... + 0);
    static constexpr auto nbSv = sizeof...(Strs);
    static constexpr std::size_t kCharsLen = len + 1U + ((nbSv == 0U ? 0U : (nbSv - 1U)) * Sep.size());
    std::array<char, kCharsLen> charsArray;
    if constexpr (len > 0) {
      auto append = [it = charsArray.begin(), &charsArray](auto const& chars) mutable {
        if (it != charsArray.begin()) {
          for (auto ch : Sep) {
            *it = ch;
            ++it;
          }
        }
        for (auto ch : chars) {
          *it = ch;
          ++it;
        }
      };
      (append(Strs), ...);
    }
    charsArray.back() = '\0';
    return charsArray;
  }
  // Give the joined string static storage
  static constexpr auto arr = impl();

 public:
  // View as a std::string_view
  static constexpr std::string_view value{arr.data(), arr.size() - 1};

  // c_str version (null-terminated)
  static constexpr const char* const c_str = arr.data();
};

// Helper to get the value out
template <std::string_view const& Sep, std::string_view const&... Strs>
inline constexpr auto JoinStringViewWithSep_v = JoinStringViewWithSep<Sep, Strs...>::value;

namespace details {
template <std::string_view const& Sep, const auto& a, typename>
struct make_joined_string_view_impl;

template <std::string_view const& Sep, const auto& a, std::size_t... i>
struct make_joined_string_view_impl<Sep, a, std::index_sequence<i...>> {
  static constexpr auto value = JoinStringViewWithSep<Sep, a[i]...>::value;
};

}  // namespace details

// make joined string view from array like value
template <std::string_view const& Sep, const auto& a>
using make_joined_string_view = details::make_joined_string_view_impl<Sep, a, std::make_index_sequence<std::size(a)>>;

/// Converts an integer value to its string_view representation at compile time.
/// The underlying storage is not null terminated.
template <int64_t intVal>
class IntToStringView {
 private:
  static constexpr auto impl() noexcept {
    std::array<char, nchars(intVal)> charsArray;
    if constexpr (intVal == 0) {
      charsArray[0] = '0';
      return charsArray;
    }
    auto endIt = charsArray.end();
    int64_t val = intVal;
    if constexpr (intVal < 0) {
      charsArray[0] = '-';
      val = -val;
    }
    do {
      *--endIt = '0' + static_cast<char>(val % 10);
      val /= 10;
    } while (val != 0);
    return charsArray;
  }

  static constexpr auto arr = impl();

 public:
  // View as a std::string_view
  static constexpr std::string_view value{arr.begin(), arr.end()};
};

template <int64_t intVal>
inline constexpr auto IntToStringView_v = IntToStringView<intVal>::value;

/// Creates a std::string_view on a storage with a single char available at compile time.
template <char Char>
class CharToStringView {
 private:
  static constexpr char ch = Char;

 public:
  static constexpr std::string_view value{&ch, 1};
};

template <char Char>
inline constexpr auto CharToStringView_v = CharToStringView<Char>::value;

}  // namespace aeronet