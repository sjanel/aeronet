#pragma once

#include <cstddef>
#include <iterator>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"

namespace aeronet {

class HeadersView {
 public:
  HeadersView() noexcept : _beg(nullptr), _end(nullptr) {}

  explicit HeadersView(std::string_view sv) noexcept : _beg(sv.data()), _end(sv.data() + sv.size()) {}

  class iterator {
   public:
    using value_type = http::HeaderView;
    using reference = http::HeaderView;
    using pointer = void;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    iterator() noexcept : _cur(nullptr), _end(nullptr), _nameLen(0), _valueLen(0) {}

    http::HeaderView operator*() const noexcept {
      return {std::string_view(_cur, _nameLen), std::string_view(_cur + _nameLen + http::HeaderSep.size(), _valueLen)};
    }

    iterator& operator++() noexcept {
      _cur += _nameLen + http::HeaderSep.size() + _valueLen + http::CRLF.size();
      if (_cur != _end) {
        setLen();
      }
      return *this;
    }

    iterator operator++(int) noexcept {
      iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    bool operator==(const iterator& rhs) const noexcept { return _cur == rhs._cur; }

   private:
    friend class HeadersView;

    iterator(const char* beg, const char* end) noexcept;

    void setLen();

    const char* _cur;
    const char* _end;
    uint32_t _nameLen;
    uint32_t _valueLen;
  };

  [[nodiscard]] iterator begin() const noexcept { return {_beg, _end}; }
  [[nodiscard]] iterator end() const noexcept { return {_end, _end}; }

 private:
  const char* _beg;
  const char* _end;
};

}  // namespace aeronet