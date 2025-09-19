#pragma once

#include <cstddef>
#include <iterator>
#include <string_view>

namespace aeronet {

/// Bi-directional iterator on {key,value} pairs of a FlatKeyValueString.
template <char KeyValuePairSep, char AssignmentChar>
class FlatKeyValueStringIterator {
  class FlatKeyValueIteratorValue {
   public:
    /// Get the key len of this iterator value
    auto keyLen() const { return _begValue - _begKey - 1U; }

    /// Get the value len of this iterator value
    auto valLen() const { return _endValue - _begValue; }

    /// Access to the key of this iterator value
    [[nodiscard]] std::string_view key() const { return {_begKey, static_cast<std::string_view::size_type>(keyLen())}; }

    /// Access to the value of this iterator value
    [[nodiscard]] std::string_view val() const { return {_begValue, _endValue}; }

    /// Get the total size of the key value pair of this iterator value
    auto size() const { return _endValue - _begKey; }

    /// a synonym of size()
    auto length() const { return size(); }

   private:
    template <char, char>
    friend class FlatKeyValueStringIterator;

    template <char, char>
    friend class FlatKeyValueString;

    /// begin()
    explicit FlatKeyValueIteratorValue(std::string_view data)
        : _begKey(data.data()), _begValue(nullptr), _endValue(nullptr) {
      std::size_t assignCharPos = data.find(AssignmentChar);
      if (assignCharPos != std::string_view::npos) {
        _begValue = _begKey + assignCharPos + 1U;

        std::size_t nextKVCharSep = data.find(KeyValuePairSep, assignCharPos + 1);
        _endValue = nextKVCharSep == std::string_view::npos ? data.data() + data.size() : data.data() + nextKVCharSep;
      }
    }

    /// end()
    explicit FlatKeyValueIteratorValue(const char *endData)
        : _begKey(endData), _begValue(nullptr), _endValue(nullptr) {}

    void incr(const char *endData) {
      if (_endValue == endData) {
        // reached the end
        _begKey = _endValue;
      } else {
        _begKey = _endValue + 1;
        _begValue = _begKey;
        do {
          ++_begValue;
        } while (*_begValue != AssignmentChar);
        ++_begValue;

        _endValue = _begValue;

        while (*_endValue != '\0' && *_endValue != KeyValuePairSep) {
          ++_endValue;
        }
      }
    }

    void decr(std::string_view data) {
      if (_begKey == data.data() + data.size()) {
        _endValue = _begKey;
      } else {
        _endValue = _begKey - 1;
      }
      _begValue = _endValue;
      do {
        --_begValue;
      } while (*_begValue != AssignmentChar);

      _begKey = _begValue;
      ++_begValue;

      do {
        --_begKey;
      } while (*_begKey != KeyValuePairSep && _begKey != data.data());

      if (*_begKey == KeyValuePairSep) {
        ++_begKey;
      }
    }

    const char *_begKey;
    const char *_begValue;
    const char *_endValue;
  };

 public:
  // Needed types for iterators.
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = FlatKeyValueIteratorValue;
  using difference_type = std::ptrdiff_t;
  using pointer = const value_type *;
  using reference = const value_type &;

  // Prefix increment, should be called on a valid iterator, otherwise undefined behavior
  FlatKeyValueStringIterator &operator++() {
    _value.incr(_data.data() + _data.size());
    return *this;
  }

  // Postfix increment
  FlatKeyValueStringIterator operator++(int) {
    auto ret = *this;
    ++(*this);
    return ret;
  }

  // Prefix decrement, should be called on a valid iterator (in range (begin(), end()]), otherwise undefined behavior
  FlatKeyValueStringIterator &operator--() {
    _value.decr(_data);
    return *this;
  }

  // Postfix decrement
  FlatKeyValueStringIterator operator--(int) {
    auto ret = *this;
    --(*this);
    return ret;
  }

  reference operator*() const { return _value; }

  pointer operator->() const { return &this->operator*(); }

  bool operator==(const FlatKeyValueStringIterator &rhs) const noexcept { return _value._begKey == rhs._value._begKey; }
  bool operator!=(const FlatKeyValueStringIterator &rhs) const noexcept { return !(*this == rhs); }

 private:
  template <char, char>
  friend class FlatKeyValueString;

  /// Create a new FlatKeyValueStringIterator representing begin()
  explicit FlatKeyValueStringIterator(std::string_view data) : _data(data), _value(data) {}

  /// Create a new FlatKeyValueStringIterator representing end()
  /// bool as second parameter is only here to differentiate both constructors
  FlatKeyValueStringIterator(std::string_view data, [[maybe_unused]] bool isEndIt)
      : _data(data), _value(_data.data() + _data.size()) {}

  std::string_view _data;
  FlatKeyValueIteratorValue _value;
};

}  // namespace aeronet