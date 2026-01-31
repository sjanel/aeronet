#include "aeronet/http-payload.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

// Notes on the NOLINT below: clang-tidy raises "bugprone-exception-escape" here because
// std::visit can potentially throw if one of the visitor operations throws. However,
// in practise it's not possible because no constructor nor move operations of HttpPayload can leave variant in a
// throwing state. Therefore we suppress the warning to keep the noexcept.

// NOLINTNEXTLINE(bugprone-exception-escape)
std::size_t HttpPayload::size() const noexcept {
  return std::visit(
      [](const auto& val) -> std::size_t {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view> ||
                      std::is_same_v<T, std::vector<char>> || std::is_same_v<T, std::vector<std::byte>> ||
                      std::is_same_v<T, RawChars>) {
          return val.size();
        } else if constexpr (std::is_same_v<T, CharBuffer> || std::is_same_v<T, BytesBuffer>) {
          return val.second;
        } else {
          return 0;
        }
      },
      _data);
}

// NOLINTNEXTLINE(bugprone-exception-escape)
char* HttpPayload::data() noexcept {
  return std::visit(
      [](auto& val) -> char* {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::vector<char>> ||
                      std::is_same_v<T, RawChars>) {
          return val.data();
        } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
          return reinterpret_cast<char*>(val.data());
        } else if constexpr (std::is_same_v<T, CharBuffer>) {
          return val.first.get();
        } else if constexpr (std::is_same_v<T, BytesBuffer>) {
          return reinterpret_cast<char*>(val.first.get());
        } else {
          return nullptr;
        }
      },
      _data);
}

// NOLINTNEXTLINE(bugprone-exception-escape)
std::string_view HttpPayload::view() const noexcept {
  return std::visit(
      [](auto const& val) -> std::string_view {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::string_view>) {
          if (val.data() == nullptr) {
            return {};
          }
          return val;
        } else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, RawChars>) {
          return val;
        } else if constexpr (std::is_same_v<T, std::vector<char>>) {
          return std::string_view(val.data(), val.size());
        } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
          return std::string_view(reinterpret_cast<const char*>(val.data()), val.size());
        } else if constexpr (std::is_same_v<T, CharBuffer>) {
          return std::string_view(val.first.get(), val.second);
        } else if constexpr (std::is_same_v<T, BytesBuffer>) {
          return std::string_view(reinterpret_cast<const char*>(val.first.get()), val.second);
        } else {
          return {};
        }
      },
      _data);
}

void HttpPayload::append(std::string_view data) {
  assert(!isFilePayload());
  std::visit(
      [this, data](auto& val) -> void {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          _data = RawChars(data);
        } else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, RawChars>) {
          val.append(data);
        } else if constexpr (std::is_same_v<T, std::vector<char>>) {
          val.insert(val.end(), data.begin(), data.end());
        } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
          val.insert(val.end(), reinterpret_cast<const std::byte*>(data.begin()),
                     reinterpret_cast<const std::byte*>(data.end()));
        } else if constexpr (std::is_same_v<T, CharBuffer> || std::is_same_v<T, BytesBuffer>) {
          // switch to RawChars to simplify appending
          RawChars rawChars(val.second + data.size());

          rawChars.unchecked_append(reinterpret_cast<const char*>(val.first.get()), val.second);
          rawChars.unchecked_append(data);

          _data = std::move(rawChars);
        } else if constexpr (std::is_same_v<T, std::string_view>) {
          // switch to RawChars to simplify appending
          RawChars rawChars(val.size() + data.size());

          rawChars.unchecked_append(val);
          rawChars.unchecked_append(data);

          _data = std::move(rawChars);
        }
      },
      _data);
}

void HttpPayload::append(const HttpPayload& other) {
  assert(!isFilePayload());
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
        } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
          const auto otherView = other.view();
          val.insert(val.end(), reinterpret_cast<const std::byte*>(otherView.data()),
                     reinterpret_cast<const std::byte*>(otherView.data() + otherView.size()));
        } else if constexpr (std::is_same_v<T, CharBuffer> || std::is_same_v<T, BytesBuffer>) {
          // switch to RawChars to simplify appending
          const auto otherView = other.view();
          RawChars rawChars(val.second + otherView.size());

          rawChars.unchecked_append(reinterpret_cast<const char*>(val.first.get()), val.second);
          rawChars.unchecked_append(otherView);

          _data = std::move(rawChars);
        } else if constexpr (std::is_same_v<T, std::string_view>) {
          // switch to RawChars to simplify appending
          const auto otherView = other.view();
          RawChars rawChars(val.size() + otherView.size());

          rawChars.unchecked_append(val);
          rawChars.unchecked_append(otherView);

          _data = std::move(rawChars);
        }
      },
      _data);
}

void HttpPayload::ensureAvailableCapacity(std::size_t capa) {
  assert(!isFilePayload());
  std::visit(
      [this, capa](auto& val) -> void {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          _data = RawChars(capa);
        } else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::vector<char>> ||
                             std::is_same_v<T, std::vector<std::byte>>) {
          val.reserve(val.size() + capa);
        } else if constexpr (std::is_same_v<T, RawChars>) {
          val.ensureAvailableCapacity(capa);
        } else if constexpr (std::is_same_v<T, CharBuffer> || std::is_same_v<T, BytesBuffer>) {
          // Switch to RawChars to simplify growth.
          const auto* beg = reinterpret_cast<const char*>(val.first.get());
          RawChars buf(val.second + capa);
          buf.unchecked_append(beg, val.second);
          _data = std::move(buf);
        } else if constexpr (std::is_same_v<T, std::string_view>) {
          // switch to RawChars to simplify appending
          RawChars rawChars(val.size() + capa);

          rawChars.unchecked_append(val);

          _data = std::move(rawChars);
        }
      },
      _data);
}

void HttpPayload::ensureAvailableCapacityExponential(std::size_t capa) {
  assert(!isFilePayload());
  std::visit(
      [this, capa](auto& val) -> void {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          _data = RawChars(capa);
        } else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::vector<char>> ||
                             std::is_same_v<T, std::vector<std::byte>>) {
#ifdef AERONET_ENABLE_ADDITIONAL_MEMORY_CHECKS
          T newVal;
          newVal.reserve(val.size() + capa);
          newVal.insert(newVal.end(), val.begin(), val.end());
          val.swap(newVal);
#else
          val.reserve(std::max(val.size() + capa, val.capacity() * 2U));
#endif
        } else if constexpr (std::is_same_v<T, RawChars>) {
          val.ensureAvailableCapacityExponential(capa);
        } else if constexpr (std::is_same_v<T, CharBuffer> || std::is_same_v<T, BytesBuffer>) {
          // Switch to RawChars to simplify growth.
          const auto* beg = reinterpret_cast<const char*>(val.first.get());
          RawChars buf(val.second + capa);
          buf.unchecked_append(beg, val.second);
          _data = std::move(buf);
        } else if constexpr (std::is_same_v<T, std::string_view>) {
          // switch to RawChars to simplify appending
          RawChars rawChars(val.size() + capa);

          rawChars.unchecked_append(val);

          _data = std::move(rawChars);
        }
      },
      _data);
}

// Inserts bytes at position 'pos'.
// May switch representation to RawChars for unsupported storage types.
void HttpPayload::insert(std::size_t pos, std::string_view data) {
  if (data.empty()) {
    return;
  }
  assert(pos <= size());
  assert(!isFilePayload());
  std::visit(
      [this, pos, data](auto& val) -> void {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          _data = RawChars(data);
        } else if constexpr (std::is_same_v<T, std::string>) {
          val.insert(pos, data.data(), data.size());
        } else if constexpr (std::is_same_v<T, std::vector<char>>) {
          val.insert(val.begin() + static_cast<std::ptrdiff_t>(pos), data.begin(), data.end());
        } else if constexpr (std::is_same_v<T, std::vector<std::byte>>) {
          val.insert(val.begin() + static_cast<std::ptrdiff_t>(pos), reinterpret_cast<const std::byte*>(data.data()),
                     reinterpret_cast<const std::byte*>(data.data() + data.size()));
        } else if constexpr (std::is_same_v<T, RawChars>) {
          val.ensureAvailableCapacityExponential(data.size());
          const auto oldSize = val.size();
          val.addSize(data.size());
          char* base = val.data();
          std::memmove(base + pos + data.size(), base + pos, oldSize - pos);
          std::memcpy(base + pos, data.data(), data.size());
        } else if constexpr (std::is_same_v<T, CharBuffer> || std::is_same_v<T, BytesBuffer>) {
          const auto* beg = reinterpret_cast<const char*>(val.first.get());
          RawChars buf(val.second + data.size());
          buf.unchecked_append(beg, pos);
          buf.unchecked_append(data);
          buf.unchecked_append(beg + pos, val.second - pos);
          _data = std::move(buf);
        } else if constexpr (std::is_same_v<T, std::string_view>) {
          // switch to RawChars to simplify appending
          RawChars buf(val.size() + data.size());
          buf.unchecked_append(val.data(), pos);  // NOLINT(bugprone-suspicious-stringview-data-usage)
          buf.unchecked_append(data);
          buf.unchecked_append(val.data() + pos, val.size() - pos);
          _data = std::move(buf);
        }
      },
      _data);
}

void HttpPayload::addSize(std::size_t sz) {
  assert(!isFilePayload());
  std::visit(
      [sz](auto& val) -> void {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::vector<char>> ||
                      std::is_same_v<T, std::vector<std::byte>>) {
          val.resize(val.size() + sz);
        } else if constexpr (std::is_same_v<T, RawChars>) {
          val.addSize(sz);
        } else {
          throw std::logic_error("Cannot call addSize on this HttpPayload representation");
        }
      },
      _data);
}

// NOLINTNEXTLINE(bugprone-exception-escape)
void HttpPayload::clear() noexcept {
  std::visit(
      [](auto& val) -> void {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::vector<char>> ||
                      std::is_same_v<T, std::vector<std::byte>> || std::is_same_v<T, RawChars>) {
          val.clear();
        } else if constexpr (std::is_same_v<T, CharBuffer> || std::is_same_v<T, BytesBuffer>) {
          val.second = 0;
        } else {
          val = {};
        }
      },
      _data);
}

void HttpPayload::shrink_to_fit() {
  std::visit(
      [](auto& val) -> void {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::vector<char>> ||
                      std::is_same_v<T, std::vector<std::byte>> || std::is_same_v<T, RawChars>) {
          val.shrink_to_fit();
        }
        // do not do anything if CharBuffer or BytesBuffer, it does not provide such functionality
      },
      _data);
}

}  // namespace aeronet