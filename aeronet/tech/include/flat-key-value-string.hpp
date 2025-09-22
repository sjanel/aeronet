#pragma once

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "cctype.hpp"
#include "flat-key-value-string-iterator.hpp"
#include "type_traits.hpp"
#include "url-encode.hpp"

namespace aeronet {

/// String Key / Value pairs flattened in a single string.
/// It can be used to store URL parameters for instance, or as an optimized key for a map / hashmap based on a list of
/// key value pairs.
/// A value can be simulated as an array of elements separated by AssignmentChar, useful for json conversion
template <char KeyValuePairSep, char AssignmentChar>
class FlatKeyValueString {
 public:
  using iterator = FlatKeyValueStringIterator<KeyValuePairSep, AssignmentChar>;
  using const_iterator = iterator;
  using size_type = std::string::size_type;

  struct KeyValuePair {
    using IntegralType = int64_t;

    std::string_view key;
    std::variant<std::string_view, IntegralType> val;
  };

  static constexpr char kArrayElemSepChar = ',';

  /// Finds the position of the given key in given data, or string::npos if key is not present
  static size_type Find(std::string_view data, std::string_view key);

  /// Get the value associated to given key, or an empty string if no value is found for this key.
  static std::string_view Get(std::string_view data, std::string_view key);

  FlatKeyValueString() noexcept = default;

  FlatKeyValueString(std::initializer_list<KeyValuePair> init)
      : FlatKeyValueString(std::span<const KeyValuePair>(init.begin(), init.end())) {}

  explicit FlatKeyValueString(std::span<const KeyValuePair> init);

  const_iterator begin() const { return const_iterator(_data); }
  const_iterator end() const { return const_iterator(_data, true); }

  const_iterator cbegin() const { return begin(); }
  const_iterator cend() const { return end(); }

  auto rbegin() const { return std::make_reverse_iterator(begin()); }
  auto rend() const { return std::make_reverse_iterator(end()); }

  auto crbegin() const { return rbegin(); }
  auto crend() const { return rend(); }

  /// Note the return by copy for front() and back()
  /// This is because operator* will return a reference to a field inside the iterator that is only temporary in this
  /// method. Returning a reference here would be dangling hence the copy (but it's cheap to copy here, only 3
  /// pointers).
  auto front() const { return *begin(); }
  auto back() const { return *(--end()); }

  /// Pushes a new {key, value} entry to the back of the FlatKeyValueString. No check is done on a duplicate key.
  /// There are several ways to set values as arrays (and none is standard). Choose the method depending on your
  /// usage:
  ///   - "aKey[]=val1&aKey[]=val2" can be used with several appends (one per value) with the same key suffixed with
  ///   []
  ///     This method needs to be used for direct call as parameter string
  ///   - If this query string will be transformed into json, set a key only once, with each value suffixed by a ','
  ///     (even the last one)
  ///     Examples:
  ///       "val": value is a single string
  ///       "val,": value is an array of a single string
  ///       "val1,val2,": value is an array of two values val1 and val2
  void emplace_back(std::string_view key, std::string_view value);

  template <std::size_t N>
  void emplace_back(std::string_view key, const std::array<char, N> &value) {
    emplace_back(key, std::string_view(value.data(), N));
  }

  void emplace_back(std::string_view key, std::integral auto val) {
    // + 1 for minus, +1 for additional partial ranges coverage
    std::array<char, std::numeric_limits<decltype(val)>::digits10 + 2> buf;

    auto [ptr, errc] = std::to_chars(buf.data(), buf.data() + buf.size(), val);

    emplace_back(key, std::string_view(buf.data(), ptr));
  }

  void push_back(const KeyValuePair &kvPair);

  /// Appends content of other FlatKeyValueString into 'this'.
  /// No check is made on duplicated keys.
  void append(const FlatKeyValueString &rhs);

  /// Pushes a new {key, value} entry at the front of this buffer.
  void emplace_front(std::string_view key, std::string_view value);

  template <std::size_t N>
  void emplace_front(std::string_view key, const std::array<char, N> &value) {
    emplace_front(key, std::string_view(value.data(), N));
  }

  void emplace_front(std::string_view key, std::integral auto val) {
    // + 1 for minus, +1 for additional partial ranges coverage
    std::array<char, std::numeric_limits<decltype(val)>::digits10 + 2> buf;

    auto [ptr, errc] = std::to_chars(buf.data(), buf.data() + buf.size(), val);

    emplace_front(key, std::string_view(buf.data(), ptr));
  }

  void push_front(const KeyValuePair &kvPair);

  /// Updates the value for given key, or append if not existing.
  void set(std::string_view key, std::string_view value);

  template <std::size_t N>
  void set(std::string_view key, const std::array<char, N> &value) {
    set(key, std::string_view(value.data(), N));
  }

  void set(std::string_view key, std::integral auto val) {
    // + 1 for minus, +1 for additional partial ranges coverage
    char buf[std::numeric_limits<decltype(val)>::digits10 + 2];
    auto [ptr, errc] = std::to_chars(buf, std::end(buf), val);

    set(key, std::string_view(buf, ptr));
  }

  /// Like emplace_back, but removes last entry if it has same key as given one.
  void set_back(std::string_view key, std::string_view value);

  template <std::size_t N>
  void set_back(std::string_view key, const std::array<char, N> &value) {
    set_back(key, std::string_view(value.data(), N));
  }

  /// Erases given key if present.
  void erase(std::string_view key);

  /// Erase key value pair at iterator.
  void erase(const_iterator it);

  /// Erases last key value pair.
  /// Undefined behavior if empty.
  void pop_back() { erase(--end()); }

  void underlyingBufferReserve(size_type capacity) { _data.reserve(capacity); }

  /// Finds the position of the given key, or string::npos if key is not present
  [[nodiscard]] size_type find(std::string_view key) const noexcept { return Find(_data, key); }

  [[nodiscard]] bool contains(std::string_view key) const noexcept { return find(key) != std::string::npos; }

  /// Get the value associated to given key, or an empty string if no value is found for this key.
  [[nodiscard]] std::string_view get(std::string_view key) const { return Get(_data, key); }

  [[nodiscard]] bool empty() const noexcept { return _data.empty(); }

  [[nodiscard]] const char *c_str() const noexcept { return _data.c_str(); }

  void clear() noexcept { _data.clear(); }

  void swap(FlatKeyValueString &rhs) noexcept { _data.swap(rhs._data); }

  /// Get a string_view on the full data hold by this FlatKeyValueString.
  /// The returned string_view is guaranteed to be null-terminated.
  [[nodiscard]] std::string_view str() const noexcept { return _data; }

  /// Converts to a json document string.
  /// Values ending with a ',' will be considered as arrays.
  /// In this case, sub array values are comma separated values.
  /// Limitation: all json values will be decoded as strings.
  [[nodiscard]] std::string toJsonStr() const;

  /// Returns a new FlatKeyValueString URL encoded except delimiters.
  FlatKeyValueString urlEncodeExceptDelimiters() const;

  auto operator<=>(const FlatKeyValueString &) const noexcept = default;

  using trivially_relocatable = is_trivially_relocatable<std::string>::type;

 private:
  explicit FlatKeyValueString(std::string &&data) noexcept(std::is_nothrow_move_constructible_v<std::string>)
      : _data(std::move(data)) {}

  std::string _data;
};

template <char KeyValuePairSep, char AssignmentChar>
FlatKeyValueString<KeyValuePairSep, AssignmentChar>::FlatKeyValueString(std::span<const KeyValuePair> init) {
  std::ranges::for_each(init, [this](const auto &kv) { push_back(kv); });
}

template <char KeyValuePairSep, char AssignmentChar>
void FlatKeyValueString<KeyValuePairSep, AssignmentChar>::emplace_back(std::string_view key, std::string_view value) {
  assert(!key.empty());
  assert(!value.empty());
  assert(key.find(KeyValuePairSep) == std::string_view::npos);
  assert(key.find(AssignmentChar) == std::string_view::npos);
  assert(value.find(KeyValuePairSep) == std::string_view::npos);

  std::size_t pos = _data.size();
  _data.append((pos == 0 ? 0U : 1U) + key.size() + 1U + value.size(), AssignmentChar);
  if (pos != 0) {
    _data[pos] = KeyValuePairSep;
    ++pos;
  }
  auto it = std::ranges::copy(key, _data.begin() + pos).out;
  std::ranges::copy(value, it + 1);
}

template <char KeyValuePairSep, char AssignmentChar>
inline void FlatKeyValueString<KeyValuePairSep, AssignmentChar>::push_back(const KeyValuePair &kv) {
  switch (kv.val.index()) {
    case 0:
      emplace_back(kv.key, std::get<std::string_view>(kv.val));
      break;
    case 1:
      emplace_back(kv.key, std::get<typename KeyValuePair::IntegralType>(kv.val));
      break;
    default:
      std::unreachable();
  }
}

template <char KeyValuePairSep, char AssignmentChar>
void FlatKeyValueString<KeyValuePairSep, AssignmentChar>::append(const FlatKeyValueString &rhs) {
  if (!rhs._data.empty()) {
    if (!_data.empty()) {
      _data.push_back(KeyValuePairSep);
    }
    _data.append(rhs._data);
  }
}

template <char KeyValuePairSep, char AssignmentChar>
void FlatKeyValueString<KeyValuePairSep, AssignmentChar>::emplace_front(std::string_view key, std::string_view value) {
  assert(!key.empty());
  assert(!value.empty());
  assert(key.find(KeyValuePairSep) == std::string_view::npos);
  assert(key.find(AssignmentChar) == std::string_view::npos);
  assert(value.find(KeyValuePairSep) == std::string_view::npos);

  if (_data.empty()) {
    _data.append(key);
    _data.push_back(AssignmentChar);
    _data.append(value);
  } else {
    _data.insert(0U, key.size() + value.size() + 2U, KeyValuePairSep);
    std::ranges::copy(key, _data.begin());
    _data[key.size()] = AssignmentChar;
    std::ranges::copy(value, _data.begin() + key.size() + 1U);
  }
}

template <char KeyValuePairSep, char AssignmentChar>
inline void FlatKeyValueString<KeyValuePairSep, AssignmentChar>::push_front(const KeyValuePair &kv) {
  switch (kv.val.index()) {
    case 0:
      emplace_front(kv.key, std::get<std::string_view>(kv.val));
      break;
    case 1:
      emplace_front(kv.key, std::get<typename KeyValuePair::IntegralType>(kv.val));
      break;
    default:
      std::unreachable();
  }
}

template <char KeyValuePairSep, char AssignmentChar>
void FlatKeyValueString<KeyValuePairSep, AssignmentChar>::set(std::string_view key, std::string_view value) {
  assert(!key.empty());
  assert(!value.empty());
  assert(key.find(KeyValuePairSep) == std::string_view::npos);
  assert(key.find(AssignmentChar) == std::string_view::npos);
  assert(value.find(KeyValuePairSep) == std::string_view::npos);

  const std::size_t pos = find(key);
  if (pos == std::string::npos) {
    emplace_back(key, value);
  } else {
    std::string::const_iterator first = _data.begin() + pos + key.size() + 1;
    std::string::const_iterator last = first + 1;
    std::string::const_iterator end = _data.end();
    while (last != end && *last != KeyValuePairSep) {
      ++last;
    }
    _data.replace(first, last, value);
  }
}

template <char KeyValuePairSep, char AssignmentChar>
void FlatKeyValueString<KeyValuePairSep, AssignmentChar>::set_back(std::string_view key, std::string_view value) {
  if (!_data.empty()) {
    auto endIt = --end();
    if (endIt->key() == key) {
      _data.replace(_data.end() - endIt->valLen(), _data.end(), value);
      return;
    }
  }

  emplace_back(key, value);
}

template <char KeyValuePairSep, char AssignmentChar>
void FlatKeyValueString<KeyValuePairSep, AssignmentChar>::erase(std::string_view key) {
  std::size_t first = find(key);
  if (first != std::string::npos) {
    if (first != 0) {
      --first;  // Remove separator char as well
    }
    std::size_t last = first + key.size() + 2;
    const std::size_t ps = _data.size();
    while (last < ps && _data[last] != KeyValuePairSep) {
      ++last;
    }
    _data.erase(_data.begin() + first, _data.begin() + last + static_cast<std::size_t>(first == 0));
  }
}

template <char KeyValuePairSep, char AssignmentChar>
void FlatKeyValueString<KeyValuePairSep, AssignmentChar>::erase(const_iterator it) {
  const char *beg = it._value._begKey;
  const char *end = it._value._endValue;

  if (end != _data.data() + _data.size()) {
    ++end;
  } else if (beg != _data.data()) {
    --beg;
  }

  _data.erase(_data.begin() + (beg - _data.data()), _data.begin() + (end - _data.data()));
}

template <char KeyValuePairSep, char AssignmentChar>
std::size_t FlatKeyValueString<KeyValuePairSep, AssignmentChar>::Find(std::string_view data, std::string_view key) {
  const std::size_t ks = key.size();
  const std::size_t ds = data.size();
  // Ideally, we would like to search for key + AssignmentChar, but we don't want to make a new string
  std::size_t pos = data.find(key);
  while (pos != std::string::npos && pos + ks < ds && data[pos + ks] != AssignmentChar) {
    pos = data.find(key, pos + ks + 1);
  }
  if (pos != std::string::npos && (pos + ks == ds || data[pos + ks] == KeyValuePairSep)) {
    // we found a value, not a key
    pos = std::string::npos;
  }
  return pos;
}

template <char KeyValuePairSep, char AssignmentChar>
std::string_view FlatKeyValueString<KeyValuePairSep, AssignmentChar>::Get(std::string_view data, std::string_view key) {
  std::size_t pos = Find(data, key);
  std::string_view::const_iterator first;
  std::string_view::const_iterator last;
  if (pos == std::string::npos) {
    first = data.end();
    last = data.end();
  } else {
    first = data.begin() + pos + key.size() + 1;
    std::size_t endPos = data.find(KeyValuePairSep, pos + key.size() + 1);
    if (endPos == std::string::npos) {
      last = data.end();
    } else {
      last = data.begin() + endPos;
    }
  }
  return {first, last};
}

template <char KeyValuePairSep, char AssignmentChar>
std::string FlatKeyValueString<KeyValuePairSep, AssignmentChar>::toJsonStr() const {
  std::string ret;
  ret.reserve(2 * (_data.size() + 1U));
  ret.push_back('{');

  const auto appendStr = [&ret](std::string_view str) {
    ret.push_back('"');
    ret.append(str);
    ret.push_back('"');
  };

  for (const auto &kv : *this) {
    const auto key = kv.key();
    const auto val = kv.val();

    if (ret.size() != 1U) {
      ret.push_back(',');
    }
    appendStr(key);
    ret.push_back(':');

    auto valSize = val.size();
    if (valSize == 0 || val.back() != kArrayElemSepChar) {
      // standard field case
      appendStr(val);
      continue;
    }

    // array case
    ret.push_back('[');

    if (valSize != 1U) {  // Check empty array case
      for (std::size_t arrayValBeg = 0;;) {
        std::size_t arrayValSepPos = val.find(kArrayElemSepChar, arrayValBeg);
        if (arrayValBeg != 0) {
          ret.push_back(',');
        }
        appendStr(std::string_view(val.begin() + arrayValBeg, val.begin() + arrayValSepPos));
        if (arrayValSepPos + 1U == valSize) {
          break;
        }
        arrayValBeg = arrayValSepPos + 1;
      }
    }

    ret.push_back(']');
  }
  ret.push_back('}');
  return ret;
}

template <char KeyValuePairSep, char AssignmentChar>
FlatKeyValueString<KeyValuePairSep, AssignmentChar>
FlatKeyValueString<KeyValuePairSep, AssignmentChar>::urlEncodeExceptDelimiters() const {
  return FlatKeyValueString<KeyValuePairSep, AssignmentChar>(URLEncode(_data, [](char ch) {
    return isalnum(ch) || ch == '@' || ch == '.' || ch == '\\' || ch == '-' || ch == '_' || ch == ':' ||
           ch == KeyValuePairSep || ch == AssignmentChar;
  }));
}

}  // namespace aeronet

namespace std {
template <char KeyValuePairSep, char AssignmentChar>
struct hash<aeronet::FlatKeyValueString<KeyValuePairSep, AssignmentChar>> {
  std::size_t operator()(const aeronet::FlatKeyValueString<KeyValuePairSep, AssignmentChar> &val) const {
    return std::hash<std::string_view>()(val.str());
  }
};
}  // namespace std
