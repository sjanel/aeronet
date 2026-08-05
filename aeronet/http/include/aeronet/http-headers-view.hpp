#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string_view>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/search-crlf.hpp"

namespace aeronet {

class HeadersView {
 public:
  HeadersView() noexcept = default;

  explicit HeadersView(std::string_view sv) noexcept : _sv(sv) {}

  [[nodiscard]] std::size_t size() const noexcept { return _sv.size(); }

  class iterator {
   public:
    using value_type = http::HeaderView;
    using reference = http::HeaderView;
    using pointer = void;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    iterator() noexcept = default;

    http::HeaderView operator*() const noexcept {
      return {{_cur, _nameLen}, {_cur + _nameLen + http::HeaderSep.size(), _valueLen}};
    }

    iterator& operator++() noexcept {
      _cur += _nameLen + http::HeaderSep.size() + _valueLen + http::CRLF.size();
      setLen();
      return *this;
    }

    iterator operator++(int) noexcept {
      const auto tmp = *this;
      ++(*this);
      return tmp;
    }

    bool operator==(const iterator& rhs) const noexcept { return _cur == rhs._cur; }

   private:
    friend class HeadersView;

    iterator(const char* beg, const char* end) noexcept : _cur(beg), _end(end) { setLen(); }

    void setLen() {
      if (_cur < _end) {
        // Use +cur + 1 to avoid issues with HTTP/2 pseudo-headers that start with ':'
        // It's not possible to have an empty header name, so there must be at least one character before the colon.
        const char* colonPtr =
            static_cast<const char*>(std::memchr(_cur + 1, ':', static_cast<std::size_t>(_end - _cur)));
        assert(colonPtr != nullptr);  // should not happen in well-formed headers
        const char* begValue = colonPtr + http::HeaderSep.size();

        _nameLen = static_cast<uint32_t>(colonPtr - _cur);
        _valueLen = static_cast<uint32_t>(SearchCRLF(begValue, _end) - begValue);
      }
    }

    const char* _cur{};
    const char* _end{};
    uint32_t _nameLen;
    uint32_t _valueLen;
  };

  [[nodiscard]] iterator begin() const noexcept { return {_sv.data(), _sv.data() + _sv.size()}; }

  [[nodiscard]] iterator end() const noexcept { return {_sv.data() + _sv.size(), _sv.data() + _sv.size()}; }

  // Optimized search for a header name with colon (case-sensitive) in the flat headers view.
  // Returns true if the header is found, false otherwise.
  [[nodiscard]] bool containsCaseSensitive(std::string_view headerNameWithColon) const noexcept {
    assert(headerNameWithColon.ends_with(http::HeaderSep));  // must include the colon

    for (std::string_view::size_type pos = 0;; pos += headerNameWithColon.size()) {
      pos = _sv.find(headerNameWithColon, pos);
      if (pos == std::string_view::npos) {
        break;
      }
      // Check that the match is at the start of a line (after CRLF or at the beginning of the string)
      if (pos == 0 ||
          (pos >= http::CRLF.size() && _sv.substr(pos - http::CRLF.size(), http::CRLF.size()) == http::CRLF)) {
        return true;
      }
    }

    return false;
  }

 private:
  std::string_view _sv;
};

}  // namespace aeronet
