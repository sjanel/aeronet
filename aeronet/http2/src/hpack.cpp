#include "aeronet/hpack.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>

#include "aeronet/http-header.hpp"
#include "aeronet/memory-utils.hpp"
#include "aeronet/mergeable-headers.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/safe-cast.hpp"
#include "aeronet/tolower-str.hpp"

namespace aeronet::http2 {

namespace {

// HPACK static table (RFC 7541 Appendix A)
// Index 0 is unused (indices are 1-based in the spec)
constexpr std::array<http::HeaderView, 61> kStaticTable = {{
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
}};

constexpr std::size_t kStaticHeaderNameMinLen =
    std::ranges::min_element(
        kStaticTable, [](const auto lhs, const auto rhs) { return lhs.size() < rhs.size(); }, &http::HeaderView::name)
        ->name.size();

constexpr std::size_t kStaticHeaderNameMaxLen =
    std::ranges::max_element(
        kStaticTable, [](const auto lhs, const auto rhs) { return lhs.size() < rhs.size(); }, &http::HeaderView::name)
        ->name.size();

// Huffman decoding table (RFC 7541 Appendix B)
// This is a simplified representation - each entry contains the symbol and the number of bits
struct HuffmanEntry {
  uint8_t symbol;
  uint8_t bitLength;
};

// Huffman encoding table (symbol -> code, length)
struct HuffmanCode {
  uint32_t code;
  uint8_t bitLength;
};

// Huffman codes from RFC 7541 Appendix B
constexpr std::array<HuffmanCode, 257> kHuffmanCodes = {{
    {0x1ff8, 13},      // 0
    {0x7fffd8, 23},    // 1
    {0xfffffe2, 28},   // 2
    {0xfffffe3, 28},   // 3
    {0xfffffe4, 28},   // 4
    {0xfffffe5, 28},   // 5
    {0xfffffe6, 28},   // 6
    {0xfffffe7, 28},   // 7
    {0xfffffe8, 28},   // 8
    {0xffffea, 24},    // 9
    {0x3ffffffc, 30},  // 10
    {0xfffffe9, 28},   // 11
    {0xfffffea, 28},   // 12
    {0x3ffffffd, 30},  // 13
    {0xfffffeb, 28},   // 14
    {0xfffffec, 28},   // 15
    {0xfffffed, 28},   // 16
    {0xfffffee, 28},   // 17
    {0xfffffef, 28},   // 18
    {0xffffff0, 28},   // 19
    {0xffffff1, 28},   // 20
    {0xffffff2, 28},   // 21
    {0x3ffffffe, 30},  // 22
    {0xffffff3, 28},   // 23
    {0xffffff4, 28},   // 24
    {0xffffff5, 28},   // 25
    {0xffffff6, 28},   // 26
    {0xffffff7, 28},   // 27
    {0xffffff8, 28},   // 28
    {0xffffff9, 28},   // 29
    {0xffffffa, 28},   // 30
    {0xffffffb, 28},   // 31
    {0x14, 6},         // 32 ' '
    {0x3f8, 10},       // 33 '!'
    {0x3f9, 10},       // 34 '"'
    {0xffa, 12},       // 35 '#'
    {0x1ff9, 13},      // 36 '$'
    {0x15, 6},         // 37 '%'
    {0xf8, 8},         // 38 '&'
    {0x7fa, 11},       // 39 '''
    {0x3fa, 10},       // 40 '('
    {0x3fb, 10},       // 41 ')'
    {0xf9, 8},         // 42 '*'
    {0x7fb, 11},       // 43 '+'
    {0xfa, 8},         // 44 ','
    {0x16, 6},         // 45 '-'
    {0x17, 6},         // 46 '.'
    {0x18, 6},         // 47 '/'
    {0x0, 5},          // 48 '0'
    {0x1, 5},          // 49 '1'
    {0x2, 5},          // 50 '2'
    {0x19, 6},         // 51 '3'
    {0x1a, 6},         // 52 '4'
    {0x1b, 6},         // 53 '5'
    {0x1c, 6},         // 54 '6'
    {0x1d, 6},         // 55 '7'
    {0x1e, 6},         // 56 '8'
    {0x1f, 6},         // 57 '9'
    {0x5c, 7},         // 58 ':'
    {0xfb, 8},         // 59 ';'
    {0x7ffc, 15},      // 60 '<'
    {0x20, 6},         // 61 '='
    {0xffb, 12},       // 62 '>'
    {0x3fc, 10},       // 63 '?'
    {0x1ffa, 13},      // 64 '@'
    {0x21, 6},         // 65 'A'
    {0x5d, 7},         // 66 'B'
    {0x5e, 7},         // 67 'C'
    {0x5f, 7},         // 68 'D'
    {0x60, 7},         // 69 'E'
    {0x61, 7},         // 70 'F'
    {0x62, 7},         // 71 'G'
    {0x63, 7},         // 72 'H'
    {0x64, 7},         // 73 'I'
    {0x65, 7},         // 74 'J'
    {0x66, 7},         // 75 'K'
    {0x67, 7},         // 76 'L'
    {0x68, 7},         // 77 'M'
    {0x69, 7},         // 78 'N'
    {0x6a, 7},         // 79 'O'
    {0x6b, 7},         // 80 'P'
    {0x6c, 7},         // 81 'Q'
    {0x6d, 7},         // 82 'R'
    {0x6e, 7},         // 83 'S'
    {0x6f, 7},         // 84 'T'
    {0x70, 7},         // 85 'U'
    {0x71, 7},         // 86 'V'
    {0x72, 7},         // 87 'W'
    {0xfc, 8},         // 88 'X'
    {0x73, 7},         // 89 'Y'
    {0xfd, 8},         // 90 'Z'
    {0x1ffb, 13},      // 91 '['
    {0x7fff0, 19},     // 92 '\'
    {0x1ffc, 13},      // 93 ']'
    {0x3ffc, 14},      // 94 '^'
    {0x22, 6},         // 95 '_'
    {0x7ffd, 15},      // 96 '`'
    {0x3, 5},          // 97 'a'
    {0x23, 6},         // 98 'b'
    {0x4, 5},          // 99 'c'
    {0x24, 6},         // 100 'd'
    {0x5, 5},          // 101 'e'
    {0x25, 6},         // 102 'f'
    {0x26, 6},         // 103 'g'
    {0x27, 6},         // 104 'h'
    {0x6, 5},          // 105 'i'
    {0x74, 7},         // 106 'j'
    {0x75, 7},         // 107 'k'
    {0x28, 6},         // 108 'l'
    {0x29, 6},         // 109 'm'
    {0x2a, 6},         // 110 'n'
    {0x7, 5},          // 111 'o'
    {0x2b, 6},         // 112 'p'
    {0x76, 7},         // 113 'q'
    {0x2c, 6},         // 114 'r'
    {0x8, 5},          // 115 's'
    {0x9, 5},          // 116 't'
    {0x2d, 6},         // 117 'u'
    {0x77, 7},         // 118 'v'
    {0x78, 7},         // 119 'w'
    {0x79, 7},         // 120 'x'
    {0x7a, 7},         // 121 'y'
    {0x7b, 7},         // 122 'z'
    {0x7ffe, 15},      // 123 '{'
    {0x7fc, 11},       // 124 '|'
    {0x3ffd, 14},      // 125 '}'
    {0x1ffd, 13},      // 126 '~'
    {0xffffffc, 28},   // 127
    {0xfffe6, 20},     // 128
    {0x3fffd2, 22},    // 129
    {0xfffe7, 20},     // 130
    {0xfffe8, 20},     // 131
    {0x3fffd3, 22},    // 132
    {0x3fffd4, 22},    // 133
    {0x3fffd5, 22},    // 134
    {0x7fffd9, 23},    // 135
    {0x3fffd6, 22},    // 136
    {0x7fffda, 23},    // 137
    {0x7fffdb, 23},    // 138
    {0x7fffdc, 23},    // 139
    {0x7fffdd, 23},    // 140
    {0x7fffde, 23},    // 141
    {0xffffeb, 24},    // 142
    {0x7fffdf, 23},    // 143
    {0xffffec, 24},    // 144
    {0xffffed, 24},    // 145
    {0x3fffd7, 22},    // 146
    {0x7fffe0, 23},    // 147
    {0xffffee, 24},    // 148
    {0x7fffe1, 23},    // 149
    {0x7fffe2, 23},    // 150
    {0x7fffe3, 23},    // 151
    {0x7fffe4, 23},    // 152
    {0x1fffdc, 21},    // 153
    {0x3fffd8, 22},    // 154
    {0x7fffe5, 23},    // 155
    {0x3fffd9, 22},    // 156
    {0x7fffe6, 23},    // 157
    {0x7fffe7, 23},    // 158
    {0xffffef, 24},    // 159
    {0x3fffda, 22},    // 160
    {0x1fffdd, 21},    // 161
    {0xfffe9, 20},     // 162
    {0x3fffdb, 22},    // 163
    {0x3fffdc, 22},    // 164
    {0x7fffe8, 23},    // 165
    {0x7fffe9, 23},    // 166
    {0x1fffde, 21},    // 167
    {0x7fffea, 23},    // 168
    {0x3fffdd, 22},    // 169
    {0x3fffde, 22},    // 170
    {0xfffff0, 24},    // 171
    {0x1fffdf, 21},    // 172
    {0x3fffdf, 22},    // 173
    {0x7fffeb, 23},    // 174
    {0x7fffec, 23},    // 175
    {0x1fffe0, 21},    // 176
    {0x1fffe1, 21},    // 177
    {0x3fffe0, 22},    // 178
    {0x1fffe2, 21},    // 179
    {0x7fffed, 23},    // 180
    {0x3fffe1, 22},    // 181
    {0x7fffee, 23},    // 182
    {0x7fffef, 23},    // 183
    {0xfffea, 20},     // 184
    {0x3fffe2, 22},    // 185
    {0x3fffe3, 22},    // 186
    {0x3fffe4, 22},    // 187
    {0x7ffff0, 23},    // 188
    {0x3fffe5, 22},    // 189
    {0x3fffe6, 22},    // 190
    {0x7ffff1, 23},    // 191
    {0x3ffffe0, 26},   // 192
    {0x3ffffe1, 26},   // 193
    {0xfffeb, 20},     // 194
    {0x7fff1, 19},     // 195
    {0x3fffe7, 22},    // 196
    {0x7ffff2, 23},    // 197
    {0x3fffe8, 22},    // 198
    {0x1ffffec, 25},   // 199
    {0x3ffffe2, 26},   // 200
    {0x3ffffe3, 26},   // 201
    {0x3ffffe4, 26},   // 202
    {0x7ffffde, 27},   // 203
    {0x7ffffdf, 27},   // 204
    {0x3ffffe5, 26},   // 205
    {0xfffff1, 24},    // 206
    {0x1ffffed, 25},   // 207
    {0x7fff2, 19},     // 208
    {0x1fffe3, 21},    // 209
    {0x3ffffe6, 26},   // 210
    {0x7ffffe0, 27},   // 211
    {0x7ffffe1, 27},   // 212
    {0x3ffffe7, 26},   // 213
    {0x7ffffe2, 27},   // 214
    {0xfffff2, 24},    // 215
    {0x1fffe4, 21},    // 216
    {0x1fffe5, 21},    // 217
    {0x3ffffe8, 26},   // 218
    {0x3ffffe9, 26},   // 219
    {0xffffffd, 28},   // 220
    {0x7ffffe3, 27},   // 221
    {0x7ffffe4, 27},   // 222
    {0x7ffffe5, 27},   // 223
    {0xfffec, 20},     // 224
    {0xfffff3, 24},    // 225
    {0xfffed, 20},     // 226
    {0x1fffe6, 21},    // 227
    {0x3fffe9, 22},    // 228
    {0x1fffe7, 21},    // 229
    {0x1fffe8, 21},    // 230
    {0x7ffff3, 23},    // 231
    {0x3fffea, 22},    // 232
    {0x3fffeb, 22},    // 233
    {0x1ffffee, 25},   // 234
    {0x1ffffef, 25},   // 235
    {0xfffff4, 24},    // 236
    {0xfffff5, 24},    // 237
    {0x3ffffea, 26},   // 238
    {0x7ffff4, 23},    // 239
    {0x3ffffeb, 26},   // 240
    {0x7ffffe6, 27},   // 241
    {0x3ffffec, 26},   // 242
    {0x3ffffed, 26},   // 243
    {0x7ffffe7, 27},   // 244
    {0x7ffffe8, 27},   // 245
    {0x7ffffe9, 27},   // 246
    {0x7ffffea, 27},   // 247
    {0x7ffffeb, 27},   // 248
    {0xffffffe, 28},   // 249
    {0x7ffffec, 27},   // 250
    {0x7ffffed, 27},   // 251
    {0x7ffffee, 27},   // 252
    {0x7ffffef, 27},   // 253
    {0x7fffff0, 27},   // 254
    {0x3ffffee, 26},   // 255
    {0x3fffffff, 30},  // 256 (EOS)
}};

// ============================
// Optimized Huffman Decode Table
// ============================
// Uses a two-level lookup table for fast decoding:
// - Level 1: 9-bit lookup (covers codes 5-9 bits, most common symbols)
// - Level 2: For longer codes, continue bit-by-bit with a state machine

// Level 1 table entry: if bits_needed <= 9, we can decode in one lookup
struct HuffmanDecodeEntry {
  uint16_t symbol;   // Decoded symbol (0-255), or 256 for EOS, or 0xFFFF if needs more bits
  uint8_t bitsUsed;  // Number of bits consumed (0 if needs more bits)
};

// Build level 1 table at compile time (512 entries for 9-bit lookup)
constexpr std::size_t kHuffmanLevel1Bits = 9;
constexpr std::size_t kHuffmanLevel1Size = 1ULL << kHuffmanLevel1Bits;

constexpr auto kHuffmanDecodeTable = []() {
  std::array<HuffmanDecodeEntry, kHuffmanLevel1Size> table;

  // Initialize all entries as "needs more bits"
  std::ranges::fill(table, HuffmanDecodeEntry{0xFFFF, 0});

  // Fill in entries for codes that fit in 9 bits or less
  for (std::size_t sym = 0; sym < 257; ++sym) {
    const auto [code, bitLen] = kHuffmanCodes[sym];

    if (bitLen <= kHuffmanLevel1Bits) {
      // This symbol can be decoded with level 1 lookup
      // The code is left-aligned in the lookup, so we need to fill all entries
      // where the top 'bitLen' bits match
      const std::size_t shift = kHuffmanLevel1Bits - bitLen;
      const std::size_t baseIdx = static_cast<std::size_t>(code) << shift;
      const std::size_t numEntries = 1ULL << shift;

      for (std::size_t iter = 0; iter < numEntries; ++iter) {
        table[baseIdx + iter].symbol = static_cast<uint16_t>(sym);
        table[baseIdx + iter].bitsUsed = bitLen;
      }
    }
  }

  return table;
}();

// For codes longer than 9 bits, we use a simple state machine approach
// with the original kHuffmanCodes table, but optimized with early exit

/// Fast path: try to decode using accumulated bits directly
constexpr uint16_t DecodeHuffmanSymbol(uint32_t code, uint8_t numBits) noexcept {
  // Binary search could be used, but linear scan with early exit is often faster
  // for the HPACK Huffman table due to good cache locality
  for (std::size_t sym = 0; sym < 257; ++sym) {
    if (kHuffmanCodes[sym].bitLength == numBits && kHuffmanCodes[sym].code == code) {
      return static_cast<uint16_t>(sym);
    }
  }
  return 0xFFFF;  // Not found
}

}  // namespace

HpackDynamicEntry::HpackDynamicEntry(std::string_view name, std::string_view value)
    : _data(std::make_unique<char[]>(name.size() + value.size())),
      _nameLength(SafeCast<uint32_t>(name.size())),
      _valueLength(SafeCast<uint32_t>(value.size())) {
  tolower_n(name.data(), name.size(), _data.get());
  Copy(value, _data.get() + name.size());
}

std::span<const http::HeaderView> GetHpackStaticTable() noexcept { return kStaticTable; }

// ============================
// HpackDynamicTable
// ============================

bool HpackDynamicTable::add(std::string_view name, std::string_view value) {
  const std::size_t entrySize = name.size() + value.size() + HpackDynamicEntry::kOverhead;

  // If entry is larger than max size, clear the table (RFC 7541 §4.4)
  if (entrySize > _maxSize) {
    clear();
    return false;
  }

  // IMPORTANT: Use name and value BEFORE any eviction, because string_views may point
  // to data owned by entries that will be evicted (use-after-free otherwise)
  HpackDynamicEntry newEntry(name, value);

  // Evict entries until there's room
  while (_currentSize + entrySize > _maxSize) {
    evict();
  }

  // Insert at the front
  _entries.insert(_entries.begin(), std::move(newEntry));
  _currentSize += entrySize;

  return true;
}

void HpackDynamicTable::setMaxSize(std::size_t maxSize) {
  _maxSize = maxSize;

  // Evict entries until we fit
  while (_currentSize > _maxSize) {
    evict();
  }
}

void HpackDynamicTable::clear() noexcept {
  _entries.clear();
  _currentSize = 0;
}

void HpackDynamicTable::evict() {
  _currentSize -= _entries.back().size();
  _entries.pop_back();
}

// ============================
// HpackDecoder
// ============================

namespace {
struct DecodedIndex {
  static constexpr std::size_t kInvalidIndex = static_cast<std::size_t>(~0ULL);

  std::size_t index{kInvalidIndex};
  std::size_t consumed{0};
};

/// Decode an integer with the specified prefix bits (RFC 7541 §5.1).
DecodedIndex DecodeInteger(std::span<const std::byte> data, uint8_t prefixBits) noexcept {
  DecodedIndex ret;

  if (data.empty()) {
    return ret;  // kInvalidIndex signals error
  }

  const uint8_t prefixMask = static_cast<uint8_t>((1U << prefixBits) - 1);
  uint64_t value = static_cast<uint8_t>(data[0]) & prefixMask;

  if (value < prefixMask) {
    // Value fits in prefix
    ret.index = value;
    ret.consumed = 1;
    return ret;
  }

  // Value requires continuation bytes
  std::size_t pos = 1;
  uint64_t multiplier = 1;

  while (pos < data.size()) {
    const uint8_t currByte = static_cast<uint8_t>(data[pos]);
    value += (currByte & 0x7F) * multiplier;
    multiplier *= 128;
    ++pos;

    if ((currByte & 0x80) == 0) {
      ret.index = value;
      ret.consumed = pos;
      return ret;
    }

    // Overflow check
    if (multiplier == 0) [[unlikely]] {
      return ret;
    }
  }

  return ret;  // Incomplete integer
}

}  // namespace

HpackDecoder::DecodeResult HpackDecoder::decode(std::span<const std::byte> data) {
  _decodedStrings.clear();
  _decodedHeadersMap.clear();

  std::size_t pos = 0;

  // Helper: decode name from index or as literal string
  auto decodeName = [this, &data, &pos](std::size_t index,
                                        const char* errMsg) -> std::pair<std::string_view, const char*> {
    if (index == 0) {
      // New literal name
      const auto nameResult = decodeString(data.subspan(pos));
      if (nameResult.consumed == DecodedString::kInvalidConsumed) {
        return {{}, errMsg};
      }
      pos += nameResult.consumed;
      return {nameResult.str, nullptr};
    }
    // Indexed name
    const auto header = lookupIndex(index);
    if (header.name.empty()) {
      return {{}, "Index out of bounds for header name"};
    }
    return {header.name, nullptr};
  };

  // Helper: decode value string
  auto decodeValue = [this, &data, &pos](const char* errMsg) -> std::pair<std::string_view, const char*> {
    auto valueResult = decodeString(data.subspan(pos));
    if (valueResult.consumed == DecodedString::kInvalidConsumed) {
      return {{}, errMsg};
    }
    pos += valueResult.consumed;
    return {valueResult.str, nullptr};
  };

  DecodeResult res{{}, _decodedHeadersMap};

  while (pos < data.size()) {
    const uint8_t firstByte = static_cast<uint8_t>(data[pos]);

    if ((firstByte & 0x80) != 0) {
      // Indexed Header Field (RFC 7541 §6.1) - Format: 1xxxxxxx
      auto indexResult = DecodeInteger(data.subspan(pos), 7);
      if (indexResult.index == DecodedIndex::kInvalidIndex) {
        res.errorMessage = "Failed to decode indexed header field index";
        return res;
      }
      pos += indexResult.consumed;

      if (indexResult.index == 0) {
        res.errorMessage = "Invalid index 0 in indexed header field";
        return res;
      }

      http::HeaderView header = lookupIndex(indexResult.index);
      if (header.name.empty()) {
        res.errorMessage = "Index out of bounds in indexed header field";
        return res;
      }

      res.errorMessage = storeHeader(header);
      if (res.errorMessage != nullptr) {
        return res;
      }

    } else if ((firstByte & 0xE0) == 0x20) {
      // Dynamic Table Size Update (RFC 7541 §6.3) - Format: 001xxxxx
      const auto sizeResult = DecodeInteger(data.subspan(pos), 5);
      if (sizeResult.index == DecodedIndex::kInvalidIndex) {
        res.errorMessage = "Failed to decode dynamic table size update";
        return res;
      }
      pos += sizeResult.consumed;

      _dynamicTable.setMaxSize(sizeResult.index);

    } else {
      // Literal Header Field - determine indexing mode and prefix bits
      const bool withIndexing = (firstByte & 0xC0) == 0x40;  // Format: 01xxxxxx
      const uint8_t prefixBits = withIndexing ? 6 : 4;

      auto indexResult = DecodeInteger(data.subspan(pos), prefixBits);
      if (indexResult.index == DecodedIndex::kInvalidIndex) {
        res.errorMessage = "Failed to decode literal header index";
        return res;
      }
      pos += indexResult.consumed;

      auto [name, nameErr] = decodeName(indexResult.index, "Failed to decode literal header name");
      if (nameErr != nullptr) {
        res.errorMessage = nameErr;
        return res;
      }

      auto [value, valueErr] = decodeValue("Failed to decode literal header value");
      if (valueErr != nullptr) {
        res.errorMessage = valueErr;
        return res;
      }

      res.errorMessage = storeHeader(http::HeaderView{name, value});
      if (res.errorMessage != nullptr) {
        return res;
      }

      if (withIndexing) {
        // Note: add() copies name/value before evicting, so this is safe even if
        // name/value point to data owned by entries that will be evicted
        _dynamicTable.add(name, value);
      }
    }
  }

  return res;
}

HpackDecoder::DecodedString HpackDecoder::decodeString(std::span<const std::byte> data) {
  auto [length, consumed] = DecodeInteger(data, 7);
  HpackDecoder::DecodedString ret({}, length + consumed);
  if (length == DecodedIndex::kInvalidIndex) {
    ret.consumed = DecodedString::kInvalidConsumed;
    return ret;
  }

  if (data.size() < consumed + length) {
    // Not enough data
    ret.consumed = DecodedString::kInvalidConsumed;
    return ret;
  }

  auto stringData = data.subspan(consumed, length);

  const bool isHuffman = (static_cast<uint8_t>(data[0]) & 0x80) != 0;
  if (isHuffman) {
    ret.str = decodeHuffman(stringData);
    if (ret.str.data() == nullptr) {  // error
      ret.consumed = DecodedString::kInvalidConsumed;
      return ret;
    }
    return ret;
  }
  // Raw string - return view directly into buffer
  ret.str = std::string_view(reinterpret_cast<const char*>(stringData.data()), stringData.size());
  return ret;
}

std::string_view HpackDecoder::decodeHuffman(std::span<const std::byte> data) {
  // Optimized Huffman decoding using two-level lookup table:
  // - Level 1: 9-bit lookup handles codes up to 9 bits (most common symbols)
  // - Level 2: Fallback to direct table scan for longer codes

  // Max decoded length for N input bytes: at most floor(8*N/5) symbols.
  const auto maxLen = SafeCast<CharStorageSizeType>((data.size() * 8U) / 5);
  char* buf = _decodedStrings.allocateAndDefaultConstruct(maxLen);
  std::size_t sz = 0;

  // Bit buffer: accumulates bits for decoding (MSB-aligned)
  // Bits are stored left-aligned: new bytes go at position (64 - 8 - bitsInBuffer)
  uint64_t bitBuffer = 0;
  int32_t bitsInBuffer = 0;
  std::size_t byteIdx = 0;

  while (byteIdx < data.size() || bitsInBuffer >= 5) {
    // Refill bit buffer - pack bytes from MSB side
    while (bitsInBuffer <= 56 && byteIdx < data.size()) {
      bitBuffer |= static_cast<uint64_t>(static_cast<uint8_t>(data[byteIdx])) << (56 - bitsInBuffer);
      bitsInBuffer += 8;
      ++byteIdx;
    }

    assert(bitsInBuffer >= 5 && "Insufficient bits in buffer while decoding Huffman symbol");

    // Try level 1 lookup if we have enough bits
    if (std::cmp_greater_equal(bitsInBuffer, kHuffmanLevel1Bits)) {
      const auto lookupBits = static_cast<std::size_t>(bitBuffer >> (64 - kHuffmanLevel1Bits));
      const auto& entry = kHuffmanDecodeTable[lookupBits];

      if (entry.bitsUsed != 0 && std::cmp_less_equal(entry.bitsUsed, bitsInBuffer)) {
        // Fast path: symbol decoded in level 1
        assert(entry.symbol != 256 && "EOS should not appear in level 1 table");
        assert(sz < maxLen);
        buf[sz++] = static_cast<char>(entry.symbol);

        bitBuffer <<= entry.bitsUsed;
        bitsInBuffer -= entry.bitsUsed;
        continue;
      }
    }

    // Slow path: need more than 9 bits, or level 1 lookup didn't match
    // Try to decode starting from the minimum bit length we can have
    bool found = false;
    const int32_t startBits = std::cmp_greater_equal(bitsInBuffer, kHuffmanLevel1Bits) ? 10 : 5;

    for (int32_t numBits = startBits; numBits <= 30 && numBits <= bitsInBuffer; ++numBits) {
      const auto code = static_cast<uint32_t>(bitBuffer >> (64 - numBits));
      const uint16_t sym = DecodeHuffmanSymbol(code, static_cast<uint8_t>(numBits));

      if (sym != 0xFFFF) {
        if (sym == 256) [[unlikely]] {
          _decodedStrings.shrinkLastAllocated(buf, 0);
          return {};  // EOS in data
        }

        assert(sz < maxLen);
        buf[sz++] = static_cast<char>(sym);

        bitBuffer <<= numBits;
        bitsInBuffer -= numBits;
        found = true;
        break;
      }
    }

    if (!found) {
      if (bitsInBuffer >= 30) {
        // We have enough bits but couldn't decode - invalid encoding
        _decodedStrings.shrinkLastAllocated(buf, 0);
        return {};
      }
      // Need more data but we've consumed all input - will check padding below
      break;
    }
  }

  // Validate remaining bits are EOS padding (all 1s, less than 8 bits)
  if (bitsInBuffer > 0) {
    if (bitsInBuffer > 7) {
      _decodedStrings.shrinkLastAllocated(buf, 0);
      return {};  // Too many leftover bits
    }
    // Check that remaining bits are all 1s (EOS prefix)
    const auto remainingBits = static_cast<uint8_t>(bitBuffer >> (64 - bitsInBuffer));
    const uint8_t expectedPadding = static_cast<uint8_t>((1U << bitsInBuffer) - 1);
    if (remainingBits != expectedPadding) {
      _decodedStrings.shrinkLastAllocated(buf, 0);
      return {};  // Invalid padding
    }
  }

  return {buf, sz};
}

http::HeaderView HpackDecoder::lookupIndex(std::size_t index) const {
  // Static table: indices 1-61
  http::HeaderView ret;
  if (index <= kStaticTable.size()) {
    const auto& entry = kStaticTable[index - 1];
    ret.name = entry.name;
    ret.value = entry.value;
    return ret;
  }

  // Dynamic table: indices 62+
  const uint32_t dynamicIndex = static_cast<uint32_t>(index - kStaticTable.size() - 1);
  if (dynamicIndex >= _dynamicTable.entryCount()) {
    return ret;
  }
  const auto& entry = _dynamicTable[dynamicIndex];
  ret.name = entry.name();
  ret.value = entry.value();
  return ret;
}

const char* HpackDecoder::storeHeader(http::HeaderView header) {
  char* headerPtr = _decodedStrings.allocateAndDefaultConstruct(SafeCast<CharStorageSizeType>(header.name.size()));
  Copy(header.name, headerPtr);

  char* valuePtr = _decodedStrings.allocateAndDefaultConstruct(SafeCast<CharStorageSizeType>(header.value.size()));
  Copy(header.value, valuePtr);

  auto [it, inserted] =
      _decodedHeadersMap.try_emplace(std::string_view(headerPtr, header.name.size()), valuePtr, header.value.size());

  if (!inserted) {
    // Header already exists
    std::string_view existingValue = it->second;

    _decodedStrings.shrinkLastAllocated(valuePtr, 0);  // valuePtr not needed anymore

    const char mergeSep = http::ReqHeaderValueSeparator(it->first, _mergeAllowedForUnknownRequestHeaders);
    if (mergeSep == '\0') {
      return "Duplicated header forbidden to merge";
    }

    const std::size_t newValueLen = existingValue.size() + 1UL + header.value.size();
    char* newValuePtr = _decodedStrings.allocateAndDefaultConstruct(SafeCast<CharStorageSizeType>(newValueLen));
    Copy(existingValue, newValuePtr);
    newValuePtr[existingValue.size()] = mergeSep;
    Copy(header.value, newValuePtr + existingValue.size() + 1UL);
    it->second = std::string_view(newValuePtr, newValueLen);
  }

  return nullptr;
}

// ============================
// HpackEncoder
// ============================

namespace {
void EncodeInteger(RawBytes& output, uint64_t value, uint8_t prefixBits, uint8_t prefixMask) {
  const uint8_t maxPrefix = static_cast<uint8_t>((1U << prefixBits) - 1);

  if (value < maxPrefix) {
    output.push_back(static_cast<std::byte>(prefixMask | static_cast<uint8_t>(value)));
    return;
  }

  output.push_back(static_cast<std::byte>(prefixMask | maxPrefix));
  value -= maxPrefix;

  while (value >= 128) {
    output.push_back(static_cast<std::byte>((value & 0x7F) | 0x80));
    value >>= 7;
  }
  output.push_back(static_cast<std::byte>(value));
}

std::size_t HuffmanEncodedLength(std::string_view str) noexcept {
  std::size_t totalBits = 0;
  for (const char ch : str) {
    totalBits += kHuffmanCodes[static_cast<uint8_t>(ch)].bitLength;
  }
  return (totalBits + 7) / 8;  // Round up to bytes
}

void EncodeHuffman(RawBytes& output, std::string_view str) {
  uint64_t currentCode = 0;
  uint8_t currentBits = 0;

  output.ensureAvailableCapacityExponential(HuffmanEncodedLength(str));

  for (char ch : str) {
    const auto code = kHuffmanCodes[static_cast<uint8_t>(ch)];
    currentCode = (currentCode << code.bitLength) | code.code;
    currentBits += code.bitLength;

    while (currentBits >= 8) {
      currentBits -= 8;
      output.unchecked_push_back(static_cast<std::byte>((currentCode >> currentBits) & 0xFF));
    }
  }

  // Pad with EOS prefix (all 1s)
  if (currentBits > 0) {
    const uint8_t padding = static_cast<uint8_t>((1U << (8 - currentBits)) - 1);
    output.unchecked_push_back(static_cast<std::byte>((currentCode << (8 - currentBits)) | padding));
  }
}

void EncodeString(RawBytes& output, std::string_view str) {
  const std::size_t huffmanLen = HuffmanEncodedLength(str);
  if (huffmanLen < str.size()) {
    // Huffman encoding is more efficient
    EncodeInteger(output, huffmanLen, 7, 0x80);
    EncodeHuffman(output, str);
    return;
  }

  // Raw string (no Huffman)
  EncodeInteger(output, str.size(), 7, 0x00);
  output.append(reinterpret_cast<const std::byte*>(str.data()), str.size());
}

}  // namespace

void HpackEncoder::encode(RawBytes& output, std::string_view name, std::string_view value, IndexingMode mode) {
  // Check for pending table size update
  if (_pendingTableSizeUpdate != std::numeric_limits<std::size_t>::max()) {
    encodeDynamicTableSizeUpdate(output, _pendingTableSizeUpdate);
    _pendingTableSizeUpdate = std::numeric_limits<std::size_t>::max();
  }

  // Try to find in tables
  auto lookup = findHeader(name, value);

  if (lookup.match == HpackLookupResult::Match::Full) {
    // Indexed Header Field (RFC 7541 §6.1)
    // Format: 1xxxxxxx
    EncodeInteger(output, lookup.index, 7, 0x80);
    return;
  }

  if (mode == IndexingMode::Indexed) {
    // Literal Header Field with Incremental Indexing (RFC 7541 §6.2.1)
    // Format: 01xxxxxx
    if (lookup.match == HpackLookupResult::Match::NameOnly) {
      EncodeInteger(output, lookup.index, 6, 0x40);
    } else {
      output.push_back(static_cast<std::byte>(0x40));
      EncodeString(output, name);
    }
    EncodeString(output, value);

    // Add to dynamic table
    _dynamicTable.add(name, value);

  } else if (mode == IndexingMode::NeverIndexed) {
    // Literal Header Field Never Indexed (RFC 7541 §6.2.3)
    // Format: 0001xxxx
    if (lookup.match == HpackLookupResult::Match::NameOnly) {
      EncodeInteger(output, lookup.index, 4, 0x10);
    } else {
      output.push_back(static_cast<std::byte>(0x10));
      EncodeString(output, name);
    }
    EncodeString(output, value);

  } else {
    // Literal Header Field without Indexing (RFC 7541 §6.2.2)
    // Format: 0000xxxx
    if (lookup.match == HpackLookupResult::Match::NameOnly) {
      EncodeInteger(output, lookup.index, 4, 0x00);
    } else {
      output.push_back(static_cast<std::byte>(0x00));
      EncodeString(output, name);
    }
    EncodeString(output, value);
  }
}

void HpackEncoder::encodeDynamicTableSizeUpdate(RawBytes& output, std::size_t newSize) {
  // Dynamic Table Size Update (RFC 7541 §6.3)
  // Format: 001xxxxx
  EncodeInteger(output, newSize, 5, 0x20);
  _dynamicTable.setMaxSize(newSize);
}

HpackLookupResult HpackEncoder::findHeader(std::string_view name, std::string_view value) {
  assert(std::ranges::none_of(name, [](char ch) { return ch >= 'A' && ch <= 'Z'; }));

  HpackLookupResult result;

  // Search static table first.
  // We could optimize this with a hash map, but linear search is acceptable
  // due to small size and good cache locality.
  if (name.size() >= kStaticHeaderNameMinLen && name.size() <= kStaticHeaderNameMaxLen) {
    auto it = std::ranges::find(kStaticTable, name, &http::HeaderView::name);
    if (it != kStaticTable.end()) {
      result.index = static_cast<uint32_t>(it - kStaticTable.begin()) + 1U;
      if (it->value == value) {
        result.match = HpackLookupResult::Match::Full;
        return result;
      }
      result.match = HpackLookupResult::Match::NameOnly;
    }
  }

  // Search dynamic table
  // TODO: would it make sense to optimize this with a hash map for large sizes?
  for (uint32_t idx = 0; idx < _dynamicTable.entryCount(); ++idx) {
    const auto& entry = _dynamicTable[idx];
    if (entry.name() == name) {
      result.index = static_cast<uint32_t>(kStaticTable.size()) + 1U + idx;
      if (entry.value() == value) {
        result.match = HpackLookupResult::Match::Full;
        return result;
      }
      result.match = HpackLookupResult::Match::NameOnly;
    }
  }

  return result;
}

}  // namespace aeronet::http2
