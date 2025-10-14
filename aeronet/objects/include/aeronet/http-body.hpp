#pragma once

#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "raw-chars.hpp"

namespace aeronet {

// Convenient wrapper of common user-types for HTTP body storage.
// The data is captured by value (moved or copied) at construction time.
// The body data is immutable after construction.
// The body view() accessor returns a std::string_view referencing the internal data.
class HttpBody {
 public:
  HttpBody() noexcept = default;

  // Constructs a HttpBody by taking ownership of the given std::string.
  explicit HttpBody(std::string str) noexcept : _data(std::move(str)) {}

  // Constructs a HttpBody by taking ownership of the given std::vector<char>.
  explicit HttpBody(std::vector<char> vec) noexcept : _data(std::move(vec)) {}

  // Constructs a HttpBody by taking ownership of the given buffer.
  explicit HttpBody(std::unique_ptr<char[]> buf, std::size_t size) noexcept : _data(CharBuffer{std::move(buf), size}) {}

  explicit HttpBody(RawChars rawChars) noexcept : _data(std::move(rawChars)) {}

  [[nodiscard]] bool set() const noexcept { return _data.index() != 0; }

  [[nodiscard]] bool unset() const noexcept { return _data.index() == 0; }

  [[nodiscard]] std::size_t size() const noexcept {
    return std::visit(
        [](auto const& val) -> std::size_t {
          using T = std::decay_t<decltype(val)>;
          if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::vector<char>> ||
                        std::is_same_v<T, RawChars>) {
            return val.size();
          } else if constexpr (std::is_same_v<T, CharBuffer>) {
            return val.second;
          } else {
            return {};
          }
        },
        _data);
  }

  [[nodiscard]] std::string_view view() const noexcept {
    return std::visit(
        [](auto const& val) -> std::string_view {
          using T = std::decay_t<decltype(val)>;
          if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, RawChars>) {
            return std::string_view(val);
          } else if constexpr (std::is_same_v<T, std::vector<char>>) {
            return std::string_view(val.data(), val.size());
          } else if constexpr (std::is_same_v<T, CharBuffer>) {
            return std::string_view(val.first.get(), val.second);
          } else {
            return {};
          }
        },
        _data);
  }

  void append(std::string_view data) {
    std::visit(
        [this, data](auto& val) -> void {
          using T = std::decay_t<decltype(val)>;
          if constexpr (std::is_same_v<T, std::monostate>) {
            _data = RawChars(data);
          } else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, RawChars>) {
            val.append(data);
          } else if constexpr (std::is_same_v<T, std::vector<char>>) {
            val.insert(val.end(), data.begin(), data.end());
          } else if constexpr (std::is_same_v<T, CharBuffer>) {
            // switch to RawChars to simplify appending
            RawChars rawChars(val.second + data.size());

            rawChars.unchecked_append(val.first.get(), val.second);
            rawChars.unchecked_append(data);

            _data = std::move(rawChars);
          }
        },
        _data);
  }

  void append(const HttpBody& other) {
    std::visit(
        [&other, this](auto& val) {
          using T = std::decay_t<decltype(val)>;
          if constexpr (std::is_same_v<T, std::monostate>) {
            _data = RawChars(other.view());
          } else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, RawChars>) {
            val.append(other.view());
          } else if constexpr (std::is_same_v<T, std::vector<char>>) {
            const auto otherView = other.view();
            val.insert(val.end(), otherView.data(), otherView.data() + otherView.size());
          } else if constexpr (std::is_same_v<T, CharBuffer>) {
            // switch to RawChars to simplify appending
            const auto otherView = other.view();
            RawChars rawChars(val.second + otherView.size());

            rawChars.unchecked_append(val.first.get(), val.second);
            rawChars.unchecked_append(otherView);

            _data = std::move(rawChars);
          }
        },
        _data);
  }

  void clear() noexcept {
    std::visit(
        [](auto& val) -> void {
          using T = std::decay_t<decltype(val)>;
          if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::vector<char>> ||
                        std::is_same_v<T, RawChars>) {
            val.clear();
          } else if constexpr (std::is_same_v<T, CharBuffer>) {
            val.second = 0;
          }
        },
        _data);
  }

 private:
  using CharBuffer = std::pair<std::unique_ptr<char[]>, std::size_t>;

  std::variant<std::monostate, std::string, std::vector<char>, CharBuffer, RawChars> _data;
};

}  // namespace aeronet