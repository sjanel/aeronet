#include "aeronet/hpack.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/city-hash.hpp"
#include "aeronet/compiler-config.hpp"
#include "aeronet/http-constants.hpp"
#include "aeronet/http-header-view.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/mergeable-headers.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/reserved-headers.hpp"
#include "aeronet/toupperlower.hpp"
#include "http2-header-is-valid.hpp"
#include "http2-read-write.hpp"

namespace aeronet::http2 {

namespace {

// HPACK static table (RFC 7541 Appendix A)
// Index 0 is unused (indices are 1-based in the spec)
constexpr http::HeaderView kStaticTable[]{
    {http::PseudoHeaderAuthority, ""},
    {http::PseudoHeaderMethod, "GET"},
    {http::PseudoHeaderMethod, "POST"},
    {http::PseudoHeaderPath, "/"},
    {http::PseudoHeaderPath, "/index.html"},
    {http::PseudoHeaderScheme, "http"},
    {http::PseudoHeaderScheme, "https"},
    {http::PseudoHeaderStatus, "200"},
    {http::PseudoHeaderStatus, "204"},
    {http::PseudoHeaderStatus, "206"},
    {http::PseudoHeaderStatus, "304"},
    {http::PseudoHeaderStatus, "400"},
    {http::PseudoHeaderStatus, "404"},
    {http::PseudoHeaderStatus, "500"},
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
    {http::ContentEncoding, ""},
    {"content-language", ""},
    {http::ContentLength, ""},
    {"content-location", ""},
    {"content-range", ""},
    {http::ContentType, ""},
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
    {http::RetryAfter, ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
};

struct StaticTableEntry {
  std::string_view name;
  std::uint8_t index;
};

static_assert(std::size(kStaticTable) <= std::numeric_limits<decltype(StaticTableEntry{}.index)>::max(),
              "Static table header names must be representable in uint8_t for efficient indexing");

// Sorted by name, then by index (to get lowest index first for name-only matches)
constexpr auto kStaticTableByName =
    [] {
      std::array<StaticTableEntry, std::size(kStaticTable)> entries{};
      for (std::uint8_t idx = 0; idx < static_cast<std::uint8_t>(std::size(kStaticTable)); ++idx) {
        entries[idx] = {kStaticTable[idx].name, idx};
      }
      std::ranges::sort(entries, [](const StaticTableEntry& lhs, const StaticTableEntry& rhs) {
        if (lhs.name != rhs.name) {
          return lhs.name < rhs.name;
        }
        return lhs.index < rhs.index;  // stable: lowest index first
      });
      return entries;
    }  // namespace
();

constexpr std::size_t kStaticHeaderNameMinLen =
    std::ranges::min_element(
        kStaticTable, [](const auto lhs, const auto rhs) { return lhs.size() < rhs.size(); }, &http::HeaderView::name)
        ->name.size();

constexpr std::size_t kStaticHeaderNameMaxLen =
    std::ranges::max_element(
        kStaticTable, [](const auto lhs, const auto rhs) { return lhs.size() < rhs.size(); }, &http::HeaderView::name)
        ->name.size();

// ============================
// Static Table Name Lookup
// ============================
// The 52 distinct names of the RFC 7541 static table are known at compile time and can never change, so this is a
// perfect-hashing problem rather than a general hashing one. The triple (length, first byte, last byte) is already
// injective over those 52 names, and multiply-shifting it with kStaticNameHashMultiplier spreads them over 128 slots
// with no collision at all (asserted below).
//
// That buys two things over hashing the whole name:
// - the slot is 3 loads plus a multiply and a shift, with no loop, so the cost no longer scales with the name length
//   and there is no serialised multiply chain;
// - a perfect hash needs no probe sequence, so a lookup is one string comparison, never a chain of them.

/// Injective key over the static table names. Only valid for a non-empty name.
constexpr uint32_t StaticNameKey(std::string_view name) noexcept {
  return static_cast<uint32_t>(name.size()) | (static_cast<uint32_t>(static_cast<uint8_t>(name.front())) << 8U) |
         (static_cast<uint32_t>(static_cast<uint8_t>(name.back())) << 16U);
}

constexpr uint32_t kStaticNameHashBits = 7U;
constexpr uint32_t kStaticNameHashSize = 1U << kStaticNameHashBits;
constexpr uint32_t kStaticNameHashMultiplier = 0x8FEBA71BU;
constexpr uint32_t kHpackOverhead = 32U;  // RFC 7541 §4.1: 32 bytes overhead per dynamic table entry

/// Slot of 'name' in kStaticNameHashTable. Only valid for a non-empty name.
constexpr uint32_t StaticNameSlot(std::string_view name) noexcept {
  return (StaticNameKey(name) * kStaticNameHashMultiplier) >> (kHpackOverhead - kStaticNameHashBits);
}

/// Entry in the static table name hash map.
/// Maps a header name to the range of matching entries in kStaticTableByName.
struct StaticNameHashEntry {
  std::string_view name;
  uint8_t sortedStart;  ///< First index in kStaticTableByName with this name
  uint8_t count;        ///< Number of consecutive entries with this name
};

constexpr auto kStaticNameHashTable = [] {
  struct Table {
    StaticNameHashEntry slots[kStaticNameHashSize]{};
    bool perfect{true};
  };
  Table ret;

  // Walk kStaticTableByName (sorted by name) and collect unique name ranges
  uint32_t idx = 0;
  while (idx < kStaticTableByName.size()) {
    const auto name = kStaticTableByName[idx].name;
    const auto start = idx;
    while (idx < kStaticTableByName.size() && kStaticTableByName[idx].name == name) {
      ++idx;
    }
    const auto slot = StaticNameSlot(name);
    if (!ret.slots[slot].name.empty()) {
      ret.perfect = false;
    }
    ret.slots[slot] = {name, static_cast<uint8_t>(start), static_cast<uint8_t>(idx - start)};
  }

  return ret;
}();

// If this ever fires, the lookup in findHeader would silently start missing names: it does not probe. Pick a new
// kStaticNameHashMultiplier that is collision-free again (any odd 32-bit constant can be searched for).
static_assert(kStaticNameHashTable.perfect,
              "kStaticNameHashMultiplier is no longer a perfect hash over the static table names");

// Huffman encoding table (symbol -> code, length)
struct HuffmanCode {
  uint32_t code;
  uint8_t bitLength;
};

// Huffman codes from RFC 7541 Appendix B
constexpr HuffmanCode kHuffmanCodes[]{
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
};

// ============================
// Huffman decoding
// ============================
// Decoding works on a 64-bit bit window, most significant bit first, and has two tiers:
// - Tier 1: the codes of 5 to 8 bits, which is 74 of the 257 symbols but well over 99% of the bytes of real header
//   text (all lowercase letters, digits and common punctuation). The window is shifted by the length of each decoded
//   code, so that this length is what the decoding of a symbol waits for from the previous one: it is computed from
//   the 5 top bits of the window, without any memory access. The symbol itself is read apart, in a table of 256 bytes
//   indexed by the top byte of the window.
// - Tier 2: canonical decoding for the 10..30 bit codes, see kHuffmanLevels below.
//
// The window is kept complemented. The zeros that a shift brings in are then 1s of the input, like the bits of the
// EOS padding that ends a string (RFC 7541 §5.2): the end of the input needs no bit count check per symbol, as it
// reads as the beginning of a code longer than 8 bits (all of them start with at least seven 1s), and leaves tier 1 the
// same way.

constexpr uint32_t kHuffmanMinBits = 5;       // Shortest code
constexpr uint32_t kHuffmanMaxShortBits = 8;  // Longest code of tier 1
constexpr uint32_t kHuffmanMaxBits = 30;      // Longest code (EOS)
constexpr uint32_t kHuffmanEosSymbol = 256;

// Tier 1, indexed by complemented bits of the window.
struct HuffmanShortCodes {
  // The symbol of the code of at most 8 bits that a window starts with, by the complement of its top byte.
  uint8_t symbols[256];
  // The length of such a code beyond kHuffmanMinBits, by the complement of the 5 top bits of the window: 2 bits each.
  uint64_t extraBits;
  // The largest complemented top byte of the windows starting with a code longer than 8 bits.
  uint8_t maxLongTop;
  // Whether the Huffman code has the properties that this representation relies on.
  bool valid;
};

constexpr HuffmanShortCodes kHuffmanShortCodes = [] {
  HuffmanShortCodes ret{};

  // The length of the short code that each top byte starts with, 0 if it starts with a longer one.
  uint8_t lengths[256]{};
  for (std::size_t sym = 0; sym < std::size(kHuffmanCodes); ++sym) {
    const auto [code, bitLen] = kHuffmanCodes[sym];
    if (bitLen > kHuffmanMaxShortBits) {
      continue;
    }
    // The window is left-aligned, so one code owns every top byte sharing its bitLen top bits.
    const std::size_t shift = kHuffmanMaxShortBits - bitLen;
    const std::size_t firstTop = static_cast<std::size_t>(code) << shift;
    for (std::size_t top = firstTop; top < firstTop + (std::size_t{1} << shift); ++top) {
      lengths[top] = bitLen;
      ret.symbols[top ^ 0xFFU] = static_cast<uint8_t>(sym);
    }
  }

  // The top bytes starting with a longer code are the largest ones.
  std::size_t firstLongTop = 0;
  while (firstLongTop < 256 && lengths[firstLongTop] != 0) {
    ++firstLongTop;
  }
  ret.valid = firstLongTop != 0 && firstLongTop < 256;
  ret.maxLongTop = static_cast<uint8_t>(firstLongTop ^ 0xFFU);

  for (std::size_t top = 0; top < 256; ++top) {
    const std::size_t extraBitsPos = 2U * ((top >> 3U) ^ 31U);
    if (top >= firstLongTop) {
      ret.valid = ret.valid && lengths[top] == 0;
    } else if ((top & 7U) == 0) {
      ret.valid = ret.valid && lengths[top] >= kHuffmanMinBits;
      ret.extraBits |= static_cast<uint64_t>(lengths[top] - kHuffmanMinBits) << extraBitsPos;
    } else {
      // The 5 top bits of the window give the length of a short code.
      ret.valid = ret.valid && lengths[top] == kHuffmanMinBits + ((ret.extraBits >> extraBitsPos) & 3U);
    }
  }

  return ret;
}();

static_assert(kHuffmanShortCodes.valid, "the tier 1 of the Huffman decoding does not fit the Huffman code");

// ---- Tier 2: canonical decoding
//
// The RFC 7541 code is canonical: within one bit length the codes are consecutive and follow symbol order, and
// firstCode[len] == (firstCode[prev] + count[prev]) << (len - prev). Consequently the MSB-aligned 64-bit windows of
// the 21 used bit lengths partition [0, 2^64) exactly, in increasing length order. So walking the lengths and taking
// the first one whose window bound covers the current window yields the right code length in at most 21 integer
// compares, instead of rescanning all 257 codes once per candidate length.
//
// Bounds are stored inclusive: the exclusive bound of the 30-bit level is 2^64 and would wrap to 0.
struct HuffmanLevel {
  uint64_t maxWindow;    ///< Largest MSB-aligned window value belonging to this bit length
  uint32_t firstCode;    ///< Smallest code of this bit length
  uint16_t symbolIndex;  ///< Index in kHuffmanSymbols of the symbol owning firstCode
  uint8_t bitLength;
};

// Symbols ordered by (bitLength, code). Symbols sharing a bit length have consecutive codes but *not* consecutive
// symbol values, so tier 2 needs this indirection rather than a base-plus-offset arithmetic.
constexpr auto kHuffmanSymbols = [] {
  std::array<uint16_t, std::size(kHuffmanCodes)> symbols{};
  std::size_t pos = 0;
  for (uint32_t bitLen = 1; bitLen <= 30; ++bitLen) {
    for (std::size_t sym = 0; sym < std::size(kHuffmanCodes); ++sym) {
      if (kHuffmanCodes[sym].bitLength == bitLen) {
        symbols[pos++] = static_cast<uint16_t>(sym);
      }
    }
  }
  return symbols;
}();

constexpr auto kHuffmanLevels = [] {
  // 21 distinct bit lengths are used; size the array to the worst case and carry the real count.
  struct Levels {
    HuffmanLevel levels[30]{};  // 1..30 bits, inclusive
    std::size_t count{};
  };
  Levels ret;

  std::size_t pos = 0;
  for (uint32_t bitLen = 1; bitLen <= 30; ++bitLen) {
    std::size_t count = 0;
    uint32_t firstCode = 0;
    for (auto kHuffmanCode : kHuffmanCodes) {
      if (kHuffmanCode.bitLength == bitLen) {
        if (count == 0) {
          firstCode = kHuffmanCode.code;
        }
        ++count;
      }
    }
    if (count == 0) {
      continue;
    }
    const auto shift = 64U - bitLen;
    const uint64_t maxWindow = (static_cast<uint64_t>(firstCode + count - 1) << shift) | ((1ULL << shift) - 1U);
    ret.levels[ret.count++] = {maxWindow, firstCode, static_cast<uint16_t>(pos), static_cast<uint8_t>(bitLen)};
    pos += count;
  }

  return ret;
}();

/// Decode one symbol of 10..30 bits from an MSB-aligned bit window (not complemented).
/// Every 64-bit window maps to exactly one symbol, so this always succeeds; invalid input is detected by the caller
/// (a code longer than the bits actually available, or EOS appearing in the data).
struct HuffmanCanonicalResult {
  uint16_t symbol;
  uint8_t bitLength;
};

constexpr HuffmanCanonicalResult DecodeHuffmanCanonical(uint64_t window) noexcept {
  static_assert(kHuffmanLevels.levels[kHuffmanLevels.count - 1].maxWindow == std::numeric_limits<uint64_t>::max(),
                "kHuffmanLevels must cover the whole window space");
  uint32_t idx = 0;
  while (true) {
    assert(idx < kHuffmanLevels.count);
    const auto& level = kHuffmanLevels.levels[idx];
    if (window <= level.maxWindow) {
      const auto code = static_cast<uint32_t>(window >> (64U - level.bitLength));
      return {kHuffmanSymbols[level.symbolIndex + (code - level.firstCode)], level.bitLength};
    }
    ++idx;
  }
}

/// Decode a Huffman string (RFC 7541 §5.2) to 'out', which has room for one symbol per kHuffmanMinBits bits of 'data',
/// plus one. Returns the end of the decoded string, or nullptr if 'data' is not a valid Huffman string.
char* DecodeHuffman(std::span<const std::byte> data, char* out) noexcept {
  // Number of short codes that the bits of a refilled window hold at least.
  static constexpr uint32_t kNbShortCodesPerRefill = 56U / kHuffmanMaxShortBits;

  const std::byte* pIn = data.data();
  const std::byte* const pEnd = pIn + data.size();

  // The complement of the bits to decode, MSB-aligned. The nbBits first ones are counted as read from the input (the
  // bytes before pIn). The bits below them are either the complement of the next bits of the input, left there by a
  // refill (OR-ing them again at the same position changes nothing), or 0: the complement of the padding.
  uint64_t window = 0;
  // A malformed string may end within a code, read partly in the padding: the count then wraps around.
  uint32_t nbBits = 0;

  if (data.size() < sizeof(uint64_t)) {
    if (data.empty()) {
      return out;
    }
    // The whole string fits in the window. Its 1 to 7 bytes are read from overlapping loads instead of a loop.
    const std::size_t size = data.size();
    const uint64_t bytes = size >= sizeof(uint32_t)
                               ? (static_cast<uint64_t>(Read32BE(pIn)) << 32U) |
                                     (static_cast<uint64_t>(Read32BE(pEnd - sizeof(uint32_t))) << (64U - (8U * size)))
                               : (static_cast<uint64_t>(pIn[0]) << 56U) |
                                     (static_cast<uint64_t>(pIn[size / 2]) << (56U - (8U * (size / 2)))) |
                                     (static_cast<uint64_t>(pEnd[-1]) << (64U - (8U * size)));
    nbBits = static_cast<uint32_t>(8U * size);
    window = ~bytes & (~uint64_t{0} << (64U - nbBits));
    pIn = pEnd;
  }

  while (true) {
    // Refill the window to at least 56 bits, unless the whole input is in it. A string is at least 8 bytes long here if
    // some of its bytes are still to read: its last ones are taken from its last 8 bytes.
    const auto nbBytesLeft = static_cast<std::size_t>(pEnd - pIn);
    if (nbBytesLeft >= sizeof(uint64_t)) {
      assert(nbBits <= 63U);
      window |= ~Read64BE(pIn) >> nbBits;
      pIn += (63U - nbBits) >> 3U;
      nbBits |= 56U;
    } else if (nbBytesLeft != 0) {
      assert(nbBits <= 63U);
      window |= (~Read64BE(pEnd - sizeof(uint64_t)) << (64U - (8U * nbBytesLeft))) >> nbBits;
      const uint32_t nbBytesRead = std::min(static_cast<uint32_t>(nbBytesLeft), (63U - nbBits) >> 3U);
      pIn += nbBytesRead;
      nbBits += 8U * nbBytesRead;
    }

    // Tier 1: a fixed number of short codes, whose only way out is a top byte starting with a longer code, which the
    // end of the input looks like.
    uint32_t nbShortCodesLeft = kNbShortCodesPerRefill;
    do {
      const auto top = static_cast<std::size_t>(window >> 56U);
      if (top <= kHuffmanShortCodes.maxLongTop) [[unlikely]] {
        break;
      }
      *out++ = static_cast<char>(kHuffmanShortCodes.symbols[top]);
      const uint64_t extraBits = (kHuffmanShortCodes.extraBits >> ((window >> 58U) & 62U)) & 3U;
      window = (window << kHuffmanMinBits) << extraBits;
      nbBits -= kHuffmanMinBits + static_cast<uint32_t>(extraBits);
    } while (--nbShortCodesLeft != 0);

    if (nbShortCodesLeft == 0) {
      continue;
    }

    // Tier 2: a longer code, or the end of the input.
    if (nbBits < kHuffmanMaxBits && pIn != pEnd) {
      continue;  // The window may not hold the whole code yet
    }
    if (nbBits < 8U) {
      // The whole input is in the window, less than a byte of it being left: the padding, which is the beginning of
      // EOS (1s only) as it is the one of a long code.
      return out;
    }
    // EOS is a decoding error (RFC 7541 §5.2). The window being all 1s past the end of the input, it is also what the
    // other malformed strings read as: a padding of 8 bits or more, and what follows a code that the input ends within
    // (its bits were counted beyond the ones of the input: the count wrapped around).
    const auto [symbol, bitLength] = DecodeHuffmanCanonical(~window);
    assert(nbBits <= 63U || symbol == kHuffmanEosSymbol);
    if (symbol == kHuffmanEosSymbol) {
      return nullptr;
    }
    *out++ = static_cast<char>(symbol);
    window <<= bitLength;
    nbBits -= bitLength;
  }
}

}  // namespace

std::span<const http::HeaderView> GetHpackStaticTable() noexcept { return kStaticTable; }

// ============================
// HpackDynamicTable
// ============================

bool HpackDynamicTable::add(std::string_view name, std::string_view value, std::size_t tag) {
  const std::size_t entrySize = name.size() + value.size() + kHpackOverhead;

  if (_maxSizeBytes < _currentSizeBytes + entrySize) {
    // If entry is larger than max size, clear the table (RFC 7541 §4.4)
    if (_maxSizeBytes < entrySize) {
      clear();
      return false;
    }

    http::Header newEntry;
    do {
      http::Header evicted = evict();
      if (newEntry.empty()) {
        // reuse memory from the evicted entry to avoid a new malloc/free pair (name may be its name: a literal field
        // whose name refers to the oldest entry)
        newEntry = http::Header(std::move(evicted), name, value);
      }
    } while (_maxSizeBytes < _currentSizeBytes + entrySize);

    // Add the entry after the eviction(s).
    _entries.emplace_back(std::move(newEntry), tag);
  } else {
    // Fast path (no eviction), construct then noexcept-move into the vector. Using push_back (noexcept move) instead of
    // emplace_back avoids exception-handling code around the malloc inside the constructor, producing tighter codegen.
    _entries.push_back(Entry{http::Header(name, value), tag});
  }

  _currentSizeBytes += entrySize;

  return true;
}

uint32_t HpackDynamicTable::findTag(uint32_t index, uint32_t endIndex, std::size_t tag,
                                    std::size_t mask) const noexcept {
  assert(index <= endIndex && endIndex <= entryCount());
  const Entry* const pEntriesEnd = _entries.data() + _entries.size();
  const Entry* const pFirst = pEntriesEnd - endIndex;
  for (const Entry* pEntry = pEntriesEnd - index; pEntry != pFirst;) {
    --pEntry;
    if (((pEntry->tag ^ tag) & mask) == 0) {
      return static_cast<uint32_t>(pEntriesEnd - 1 - pEntry);
    }
  }
  return endIndex;
}

void HpackDynamicTable::setMaxSize(uint32_t maxSize) {
  _maxSizeBytes = maxSize;

  // Evict entries until we fit
  while (_maxSizeBytes < _currentSizeBytes) {
    evict();
  }
}

void HpackDynamicTable::clear() noexcept {
  _entries.clear();
  _firstLive = 0;
  _currentSizeBytes = 0;
}

http::Header HpackDynamicTable::evict() {
  assert(_firstLive < _entries.size());

  // The oldest entry sits at the cursor; retiring it is a move plus a cursor bump.
  http::Header evictedEntry = std::move(_entries[_firstLive].header);
  _currentSizeBytes -= evictedEntry.size() + kHpackOverhead;
  ++_firstLive;

  if (_firstLive == _entries.size()) {
    // Table is empty again - reset rather than leave a retired prefix behind.
    _entries.clear();
    _firstLive = 0;
  } else if (_firstLive >= _entries.size() - _firstLive) {
    // Retired prefix has grown to at least the size of the live range. Reclaiming it here bounds the array at twice
    // the live entry count and amortises to one relocation per add.
    // Drop the retired prefix, moving the live entries back to offset 0.
    _entries.erase(_entries.begin(), _entries.begin() + _firstLive);
    _firstLive = 0;
  }

  return evictedEntry;
}

// ============================
// HpackDecoder
// ============================

namespace {
struct DecodedIndex {
  static constexpr uint32_t kInvalidIndex = std::numeric_limits<uint32_t>::max();

  uint32_t index{kInvalidIndex};
  uint8_t consumed{0};
};

/// Decode an integer with the specified prefix bits (RFC 7541 §5.1).
DecodedIndex DecodeInteger(std::span<const std::byte> data, uint8_t prefixBits) noexcept {
  DecodedIndex ret;

  if (data.empty()) {
    return ret;  // kInvalidIndex signals error
  }

  const uint8_t prefixMask = static_cast<uint8_t>((1U << prefixBits) - 1);
  uint64_t value = static_cast<uint64_t>(static_cast<uint8_t>(data[0]) & prefixMask);

  if (value < prefixMask) {
    // Value fits in prefix
    ret.index = static_cast<uint32_t>(value);
    ret.consumed = 1;
    return ret;
  }

  // Value requires continuation bytes.
  // Security hardening: cap at 8 continuation bytes (enough for any valid uint64_t / HTTP/2 value).
  // This prevents a malicious encoder from forcing excessive loop iterations with crafted
  // continuation bytes before the original multiplier-overflow check would trigger.
  static constexpr uint8_t kMaxContinuationBytes = 8;
  uint8_t pos = 1;
  uint64_t multiplier = 1;

  while (pos < data.size()) {
    if (pos > kMaxContinuationBytes) [[unlikely]] {
      return ret;  // Invalid: integer encoding uses too many continuation bytes
    }

    const uint8_t currByte = static_cast<uint8_t>(data[pos]);
    value += (currByte & 0x7FU) * multiplier;

    // Overflow cannot happen, we can only multiply by 128 (2⁷) kMaxContinuationBytes times.
    static_assert(kMaxContinuationBytes * 7 < static_cast<uint8_t>(sizeof(uint64_t) * 8),
                  "Multiplier overflow possible in DecodeInteger");

    multiplier *= 128U;
    ++pos;

    if ((currByte & 0x80U) == 0) {
      // A value not fitting in 32 bits exceeds the limits of the implementation: a decoding error (RFC 7541 §5.1),
      // instead of a truncated value.
      if (value < DecodedIndex::kInvalidIndex) {
        ret.index = static_cast<uint32_t>(value);
        ret.consumed = pos;
      }
      break;
    }
  }

  return ret;
}

// The PseudoHeader of a pseudo-header field name, or std::size(kPseudoHeaderNames) for an undefined one.
[[nodiscard]] std::size_t PseudoHeaderIndex(std::string_view name) noexcept {
  return static_cast<std::size_t>(std::ranges::find(kPseudoHeaderNames, name) - kPseudoHeaderNames);
}

}  // namespace

void DecodedHeaders::clear() noexcept {
  headers.clear();
  storage.clear();
  std::ranges::fill(pseudoHeaders, std::string_view());
  namesAndValuesSize = 0;
  pseudoHeadersMask = 0;
  hasConnectionSpecificField = false;
}

HpackDecoder::HpackDecoder(uint32_t maxDynamicTableSize, uint32_t maxHeaderListSize,
                           bool mergeAllowedForUnknownRequestHeaders, Messages messages) noexcept
    : _dynamicTable(maxDynamicTableSize),
      _maxHeaderListSize(maxHeaderListSize),
      _dynamicTableSizeLimit(maxDynamicTableSize),
      _mergeAllowedForUnknownRequestHeaders(mergeAllowedForUnknownRequestHeaders),
      _messages(messages) {}

void HpackDecoder::setMaxDynamicTableSize(uint32_t maxSize) {
  _dynamicTableSizeLimit = maxSize;
  // The encoder has to start its next header block with a size update not above the reduced limit (RFC 9113 §4.3.1),
  // which evicts the same entries.
  if (maxSize < _dynamicTable.maxSizeBytes()) {
    _dynamicTable.setMaxSize(maxSize);
  }
}

HpackDecoder::DecodeResult HpackDecoder::decode(std::span<const std::byte> data) {
  _decodedHeaders.clear();
  _deferredJoins.clear();

  std::size_t pos = 0;

  DecodeResult res{_decodedHeaders, 0UL};

  auto recordFieldError = [&res](DecodeResult::Error err) {
    if (res.error == DecodeResult::Error::None) {
      res.error = err;
    }
  };

  bool seenRegularHeader = false;

  // A refused block leaves the storage no larger than it found it (see the end).
  const DecodedHeaders::Storage::size_type storageCapacity = _decodedHeaders.storage.capacity();

  // The fields beyond the header list size limit are not stored, the block being refused: storing them could take far
  // more memory than the block itself, each of its bytes possibly referencing a large field of the dynamic table.
  // plainNameHash: see storeHeader(). It is set to 0 for a field that is not stored, which is not checked.
  // Returns whether the field is within the limit.
  const auto storeField = [&](std::string_view name, Source nameSource, std::string_view value, Source valueSource,
                              std::size_t& plainNameHash) {
    res.headerListSize += name.size() + value.size() + kHpackOverhead;
    if (res.headerListSize <= _maxHeaderListSize) {
      recordFieldError(storeHeader(name, nameSource, value, valueSource, seenRegularHeader, plainNameHash));
      return true;
    }
    plainNameHash = 0;
    return false;
  };

  while (pos < data.size()) {
    const uint8_t firstByte = static_cast<uint8_t>(data[pos]);

    if ((firstByte & 0x80U) != 0) {
      // Indexed Header Field (RFC 7541 §6.1) - Format: 1xxxxxxx
      const auto indexResult = DecodeInteger(data.subspan(pos), 7);
      if (indexResult.index == DecodedIndex::kInvalidIndex) {
        res.error = DecodeResult::Error::FailedToDecodeIndexedHeaderFieldIndex;
        return res;
      }
      pos += indexResult.consumed;

      if (indexResult.index == 0) {
        res.error = DecodeResult::Error::InvalidIndexZeroInIndexedHeaderField;
        return res;
      }

      auto [header, plainNameHash, source] = lookupIndex(indexResult.index);
      if (header.name.data() == nullptr) {
        res.error = DecodeResult::Error::IndexOutOfBoundsInIndexedHeaderField;
        return res;
      }

      if (plainNameHash != 0) {
        // The common case on an established connection: a field that the peer sent before, and indexed.
        res.headerListSize += header.name.size() + header.value.size() + kHpackOverhead;
        if (res.headerListSize <= _maxHeaderListSize) {
          seenRegularHeader = true;
          recordFieldError(storePlainField(header.name, header.value, plainNameHash));
        }
      } else {
        storeField(header.name, source, header.value, source, plainNameHash);
      }

    } else if ((firstByte & 0xE0U) == 0x20U) {
      // Dynamic Table Size Update (RFC 7541 §6.3) - Format: 001xxxxx
      // Only at the beginning of a header block (RFC 7541 §4.2): each decoded field adds at least 32 bytes to the list.
      if (res.headerListSize != 0) {
        res.error = DecodeResult::Error::DynamicTableSizeUpdateAfterField;
        return res;
      }
      const auto sizeResult = DecodeInteger(data.subspan(pos), 5);
      if (sizeResult.index == DecodedIndex::kInvalidIndex) {
        res.error = DecodeResult::Error::FailedToDecodeDynamicTableSizeUpdate;
        return res;
      }
      pos += sizeResult.consumed;

      // Without this check, the encoder could make the table hold up to 4 GiB, whatever the advertised limit.
      if (sizeResult.index > _dynamicTableSizeLimit) {
        res.error = DecodeResult::Error::DynamicTableSizeUpdateAboveLimit;
        return res;
      }

      _dynamicTable.setMaxSize(sizeResult.index);

    } else {
      // Literal Header Field - determine indexing mode and prefix bits
      const bool withIndexing = (firstByte & 0xC0U) == 0x40U;  // Format: 01xxxxxx
      const uint8_t prefixBits = withIndexing ? 6U : 4U;

      auto indexResult = DecodeInteger(data.subspan(pos), prefixBits);
      if (indexResult.index == DecodedIndex::kInvalidIndex) {
        res.error = DecodeResult::Error::FailedToDecodeLiteralHeaderIndex;
        return res;
      }
      pos += indexResult.consumed;

      std::string_view name;
      Source nameSource;
      std::size_t plainNameHash = 0;
      if (indexResult.index == 0) {
        // New literal name
        const auto nameResult = decodeString(data.subspan(pos));
        if (nameResult.consumed == DecodedString::kInvalidConsumed) {
          res.error = DecodeResult::Error::FailedToDecodeLiteralHeaderName;
          return res;
        }
        pos += nameResult.consumed;
        name = nameResult.str;
        nameSource = nameResult.source;
      } else {
        // Indexed name
        const IndexedField indexedField = lookupIndex(indexResult.index);
        if (indexedField.header.name.data() == nullptr) {
          res.error = DecodeResult::Error::IndexOutOfBoundsForHeaderName;
          return res;
        }
        name = indexedField.header.name;
        nameSource = indexedField.source;
        plainNameHash = indexedField.plainNameHash;
      }

      const auto valueResult = decodeString(data.subspan(pos));
      if (valueResult.consumed == DecodedString::kInvalidConsumed) {
        res.error = DecodeResult::Error::FailedToDecodeLiteralHeaderValue;
        return res;
      }
      pos += valueResult.consumed;

      const bool stored = storeField(name, nameSource, valueResult.str, valueResult.source, plainNameHash);

      if (withIndexing) {
        // The name may point into an entry that add() evicts, even into the oldest one whose buffer it reuses for the
        // new entry: add() supports it. Dynamic table must be updated even in case of header validation issue - cf. RFC
        // 9113 §4.3. The entry is tagged with the hash of its name if it is a plain field (see storePlainField()), and
        // with 0 otherwise.
        _dynamicTable.add(name, valueResult.str, plainNameHash);
      }

      if (!stored) {
        // The decoded Huffman strings of a field beyond the limit are given back to the storage, the table holding its
        // own copy: the strings of the next fields take their place instead of piling up. The name was decoded right
        // before the value, unless the value needed a new block of the storage.
        DecodedHeaders::Storage& storage = _decodedHeaders.storage;
        if (nameSource == Source::Storage &&
            (valueResult.source != Source::Storage || valueResult.str.data() == name.data() + name.size())) {
          // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
          storage.shrinkLastAllocated(name.data(), 0);
        } else if (valueResult.source == Source::Storage) {
          // NOLINTNEXTLINE(bugprone-suspicious-stringview-data-usage)
          storage.shrinkLastAllocated(valueResult.str.data(), 0);
        }
      }
    }
  }

  if (res.headerListSize > _maxHeaderListSize) {
    recordFieldError(DecodeResult::Error::HeaderListTooLarge);
    if (_decodedHeaders.storage.capacity() > storageCapacity) {
      // The longest Huffman string of a field beyond the limit (8/5 of its encoded size once decoded) may have made the
      // storage grow: its fields being refused, it is released rather than kept for the next blocks.
      _decodedHeaders.clear();
      _decodedHeaders.storage = DecodedHeaders::Storage(DecodedHeaders::kStorageInitialCapacity);
    }
  } else if (res.isSuccess() && !_deferredJoins.empty()) {
    joinRepeatedFields();
  }

  return res;
}

HpackDecoder::DecodedString HpackDecoder::decodeString(std::span<const std::byte> data) {
  const auto [length, consumed] = DecodeInteger(data, 7);
  HpackDecoder::DecodedString ret;
  // Not 'data.size() < consumed + length': computed in 32 bits, the sum wraps around for a length close to 2^32, and
  // the string would extend far beyond the block.
  if (length == DecodedIndex::kInvalidIndex || data.size() - consumed < length) {
    return ret;  // Invalid length, or not enough data
  }
  ret.consumed = consumed + static_cast<std::size_t>(length);

  const auto stringData = data.subspan(consumed, length);

  const bool isHuffman = (static_cast<uint8_t>(data[0]) & 0x80U) != 0;
  if (isHuffman) {
    ret.str = decodeHuffman(stringData);
    if (ret.str.data() == nullptr) {  // error
      ret.consumed = DecodedString::kInvalidConsumed;
      return ret;
    }
    ret.source = Source::Storage;
    return ret;
  }
  // Raw string - return view directly into buffer
  ret.str = std::string_view(reinterpret_cast<const char*>(stringData.data()), stringData.size());
  return ret;
}

std::string_view HpackDecoder::decodeHuffman(std::span<const std::byte> data) {
  // At most one symbol per kHuffmanMinBits bits, plus the one that a malformed string may end with (see DecodeHuffman).
  const auto maxLen = ((data.size() * 8U) / kHuffmanMinBits) + 1U;
  DecodedHeaders::Storage& storage = _decodedHeaders.storage;
  char* const buf = storage.allocateAndDefaultConstruct(maxLen);
  const char* const end = DecodeHuffman(data, buf);
  if (end == nullptr) {
    storage.shrinkLastAllocated(buf, 0);
    return {};
  }

  // The decoded string stays in the storage, the decoded headers pointing to it: give back the unused part.
  const auto sz = static_cast<std::size_t>(end - buf);
  assert(sz < maxLen);
  storage.shrinkLastAllocated(buf, sz);
  return {buf, sz};
}

HpackDecoder::IndexedField HpackDecoder::lookupIndex(uint32_t index) const {
  assert(index != 0);

  // Static table: indices 1-61
  if (index <= std::size(kStaticTable)) {
    return {kStaticTable[index - 1U], 0, Source::StaticTable};
  }

  // Dynamic table: indices 62+. Its entries may be evicted while the decoded headers are still in use.
  const uint32_t dynamicIndex = static_cast<uint32_t>(index - std::size(kStaticTable) - 1U);
  if (dynamicIndex >= _dynamicTable.entryCount()) {
    return {};
  }
  const auto& entry = _dynamicTable[dynamicIndex];
  return {{entry.name(), entry.value()}, _dynamicTable.tag(dynamicIndex), Source::Transient};
}

std::string_view HpackDecoder::stableString(std::string_view str, Source source) {
  if (source != Source::Transient) {
    return str;
  }
  char* pStr = _decodedHeaders.storage.allocateAndDefaultConstruct(str.size());
  Copy(str, pStr);
  return {pStr, str.size()};
}

HpackDecoder::DecodeResult::Error HpackDecoder::storeHeader(std::string_view name, Source nameSource,
                                                            std::string_view value, Source valueSource,
                                                            bool& seenRegularHeader, std::size_t& plainNameHash) {
  // The hash of the name when it is already known: the name is then the one of a plain field.
  const std::size_t knownNameHash = plainNameHash;
  plainNameHash = 0;

  // The fields of the static table are valid, like the names of the plain fields.
  if (nameSource != Source::StaticTable && knownNameHash == 0 && !IsValidHTTP2HeaderName(name)) {
    return DecodeResult::Error::MalformedFieldName;
  }
  assert(IsValidHTTP2HeaderName(name));
  assert(knownNameHash == 0 || (name.front() != ':' && knownNameHash == CityHash{}(name)));

  if (name.front() == ':') {  // pseudo header
    if (seenRegularHeader) {
      return DecodeResult::Error::PseudoHeaderFieldAfterRegularField;
    }
    const std::size_t pseudoHeaderIdx = PseudoHeaderIndex(name);
    if (pseudoHeaderIdx == std::size(kPseudoHeaderNames)) {
      return DecodeResult::Error::UndefinedPseudoHeaderField;
    }
    const auto pseudoHeader = static_cast<PseudoHeader>(pseudoHeaderIdx);
    if (_decodedHeaders.hasPseudoHeader(pseudoHeader)) {
      return DecodeResult::Error::DuplicatePseudoHeaderField;
    }
    if (valueSource != Source::StaticTable && !IsValidHTTP2HeaderValue(value)) {
      return DecodeResult::Error::MalformedFieldValue;
    }
    // The server decodes the :path value in place: it must not stay in the static table ('/' or '/index.html').
    if (pseudoHeader == PseudoHeader::Path) {
      valueSource = Source::Transient;
    }
    _decodedHeaders.pseudoHeaders[pseudoHeaderIdx] = stableString(value, valueSource);
    _decodedHeaders.pseudoHeadersMask |= DecodedHeaders::PseudoHeaderBit(pseudoHeader);
    _decodedHeaders.namesAndValuesSize += name.size() + value.size();
    return DecodeResult::Error::None;
  }

  seenRegularHeader = true;

  if (valueSource != Source::StaticTable && !IsValidHTTP2HeaderValue(value)) {
    return DecodeResult::Error::MalformedFieldValue;
  }

  bool isPlainField = true;
  if (http::IsConnectionSpecificField(name, value)) [[unlikely]] {
    // A handler forwarding such a request to an HTTP/1.1 backend would open a request smuggling path
    // (transfer-encoding).
    if (_messages == Messages::Requests) {
      return DecodeResult::Error::ConnectionSpecificField;
    }
    _decodedHeaders.hasConnectionSpecificField = true;
    isPlainField = false;
  }

  const bool isContentEncoding = name == http::ContentEncoding;
  if (isContentEncoding) {
    // Content codings are case-insensitive (RFC 9110 §8.4.1): their value is lower-cased once stored. It is always
    // copied for this, as a decoded Huffman string may also be added to the dynamic table, which keeps the received
    // value.
    valueSource = Source::Transient;
    isPlainField = false;
  }
  // The name is hashed where it is, before its copy to the storage: a load from bytes that were just copied waits for
  // them.
  const std::size_t nameHash = knownNameHash != 0 ? knownNameHash : CityHash{}(name);
  const std::string_view storedValue = stableString(value, valueSource);
  if (isContentEncoding) {
    // A plain loop is enough for these few bytes (the vectorized tolower() of tolower-str.hpp would bring
    // <immintrin.h> into this file).
    char* pValue = const_cast<char*>(storedValue.data());
    std::transform(pValue, pValue + storedValue.size(), pValue, [](char ch) { return tolower(ch); });
  }

  const auto [it, inserted] =
      _decodedHeaders.headers.try_emplace_hashed(nameHash, stableString(name, nameSource), storedValue);
  if (!inserted) [[unlikely]] {
    const DecodeResult::Error err = mergeRepeatedField(it, storedValue);
    if (err != DecodeResult::Error::None) {
      return err;
    }
  }

  _decodedHeaders.namesAndValuesSize += name.size() + value.size();
  if (isPlainField) {
    plainNameHash = nameHash;
  }
  return DecodeResult::Error::None;
}

HpackDecoder::DecodeResult::Error HpackDecoder::storePlainField(std::string_view name, std::string_view value,
                                                                std::size_t nameHash) {
  assert(IsValidHTTP2HeaderName(name) && name.front() != ':' && IsValidHTTP2HeaderValue(value));
  assert(!http::IsConnectionSpecificField(name, value) && name != http::ContentEncoding);
  assert(nameHash == CityHash{}(name));

  // The entry of the dynamic table may be evicted while the decoded headers are in use: its name and its value are
  // copied to the storage, in one allocation.
  char* pName = _decodedHeaders.storage.allocateAndDefaultConstruct(name.size() + value.size());
  char* pValue = Append(name, pName);
  Copy(value, pValue);
  const std::string_view storedValue(pValue, value.size());

  const auto [it, inserted] =
      _decodedHeaders.headers.try_emplace_hashed(nameHash, std::string_view(pName, name.size()), storedValue);
  if (!inserted) [[unlikely]] {
    const DecodeResult::Error err = mergeRepeatedField(it, storedValue);
    if (err != DecodeResult::Error::None) {
      return err;
    }
  }

  _decodedHeaders.namesAndValuesSize += name.size() + value.size();
  return DecodeResult::Error::None;
}

HpackDecoder::DecodeResult::Error HpackDecoder::mergeRepeatedField(SvToSvMap::iterator it,
                                                                   std::string_view storedValue) {
  // Header already exists: same rules as for HTTP/1.1 (see http::MergeHeaderInPlace()), the cookie fields being joined
  // with "; " (RFC 9113 §8.2.3), except for the Set-Cookie fields of a response, which the client splits back.
  const char mergeSep = _messages == Messages::Responses && it->first == http::SetCookie
                            ? DecodedHeaders::kSetCookieSeparator
                            : http::ReqHeaderValueSeparator(it->first, _mergeAllowedForUnknownRequestHeaders);
  if (mergeSep == '\0') {
    return DecodeResult::Error::DuplicateHeaderForbiddenToMerge;
  }

  if (it->second.empty()) {
    // The first value is empty: keep the last one
    it->second = storedValue;
  } else if (!storedValue.empty()) {
    // Joined once the whole block is decoded: joining each value as it comes would copy the previous ones again each
    // time, a quadratic cost for a block repeating a field.
    _deferredJoins.push_back(
        DeferredJoin{it->first, storedValue, static_cast<uint32_t>(_deferredJoins.size()), mergeSep});
  }
  return DecodeResult::Error::None;
}

void HpackDecoder::joinRepeatedFields() {
  // Group the values to append by field (all the values of a field refer to its single name view, distinct from the
  // other fields), in their order. An in-place sort with an explicit order: a stable sort would allocate.
  std::ranges::sort(_deferredJoins, [](const DeferredJoin& lhs, const DeferredJoin& rhs) {
    if (lhs.name.data() != rhs.name.data()) {
      return lhs.name.data() < rhs.name.data();
    }
    return lhs.order < rhs.order;
  });
  for (auto first = _deferredJoins.begin(); first != _deferredJoins.end();) {
    const char* pName = first->name.data();
    const auto last = std::find_if(first, _deferredJoins.end(), [pName](const DeferredJoin& deferredJoin) {
      return deferredJoin.name.data() != pName;
    });

    std::string_view& value = _decodedHeaders.headers.find(first->name)->second;
    std::size_t joinedSize = value.size();
    for (auto it = first; it != last; ++it) {
      joinedSize += http::MergeSeparatorSize(it->separator) + it->value.size();
    }

    char* pJoined = _decodedHeaders.storage.allocateAndDefaultConstruct(joinedSize);
    char* pData = Append(value, pJoined);
    for (auto it = first; it != last; ++it) {
      pData = Append(it->value, http::AppendMergeSeparator(it->separator, pData));
    }
    value = std::string_view(pJoined, joinedSize);
    first = last;
  }
}

// ============================
// HpackEncoder
// ============================

namespace {

constexpr std::size_t kDynamicIndexMinEntries = 128U;

// Number of entries of the dynamic table of an encoder, from the newest one, that a lookup scans before trying the
// entry where the field was last found.
constexpr uint32_t kNbNewestEntriesScannedFirst = 4U;

template <class T>
[[nodiscard]] T LoadNative(const char* pData) noexcept {
  T ret;
  std::memcpy(&ret, pData, sizeof(T));
  return ret;
}

// 32 bits from the size and the first and last 8 bytes of a string (all its bytes if it is shorter): what differs
// between the names, and between the values of a name, in the dynamic table of an encoder (dates, identifiers...).
[[nodiscard]] uint32_t StringFingerprint(std::string_view str) noexcept {
  const char* const pData = str.data();
  const std::size_t size = str.size();
  uint64_t edges = 0;
  if (size >= sizeof(uint64_t)) {
    edges = LoadNative<uint64_t>(pData) ^ std::rotl(LoadNative<uint64_t>(pData + size - sizeof(uint64_t)), 32);
  } else if (size >= sizeof(uint32_t)) {
    edges = LoadNative<uint32_t>(pData) |
            (static_cast<uint64_t>(LoadNative<uint32_t>(pData + size - sizeof(uint32_t))) << 32U);
  } else if (size != 0) {
    edges = static_cast<uint64_t>(static_cast<uint8_t>(pData[0])) |
            (static_cast<uint64_t>(static_cast<uint8_t>(pData[size / 2U])) << 8U) |
            (static_cast<uint64_t>(static_cast<uint8_t>(pData[size - 1])) << 16U);
  }
  // The high half of the product depends on every bit of its factor.
  return static_cast<uint32_t>(((edges ^ size) * 0x9E3779B97F4A7C15ULL) >> 32U);
}

// The fingerprint of a field, kept as the tag of its entry in the dynamic table of an encoder: a lookup compares the
// strings of the entries whose tag has the half of the name of the field only. Its high half is the one of the name.
// Out of line, as its two call sites would otherwise grow the lookup of the fields that repeat.
[[nodiscard]] AERONET_NOINLINE std::size_t FieldFingerprint(std::string_view name, std::string_view value) noexcept {
  return (static_cast<std::size_t>(StringFingerprint(name)) << 32U) | StringFingerprint(value);
}

constexpr std::size_t kNameFingerprintMask = ~std::size_t{0} << 32U;

[[nodiscard]] uint32_t HpackDynamicTableIndex(std::size_t tableIdx) noexcept {
  return static_cast<uint32_t>(std::size(kStaticTable) + 1U + tableIdx);
}

[[nodiscard]] HpackLookupResult FindDynamicHeaderLinear(const HpackDynamicTable& table, std::string_view name,
                                                        std::string_view value) {
  HpackLookupResult result;
  for (std::size_t tableIdx = 0; tableIdx < table.entryCount(); ++tableIdx) {
    const http::Header& entry = table[static_cast<uint32_t>(tableIdx)];
    if (entry.name() != name) {
      continue;
    }
    if (result.match == HpackLookupResult::Match::None) {
      result = {HpackLookupResult::Match::NameOnly, HpackDynamicTableIndex(tableIdx)};
    }
    if (entry.value() == value) {
      return {HpackLookupResult::Match::Full, HpackDynamicTableIndex(tableIdx)};
    }
  }
  return result;
}

}  // namespace

std::size_t HpackEncoder::dynamicFullHash(std::size_t nameHash, std::string_view value) noexcept {
  const auto valueHash = CityHash{}(value);
  return nameHash ^ (valueHash + static_cast<std::size_t>(0x9e3779b97f4a7c15ULL) + (nameHash << 6U) + (nameHash >> 2U));
}

std::size_t HpackEncoder::dynamicNameHash(std::size_t nameHash) noexcept {
  return nameHash ^ static_cast<std::size_t>(0xd6e8feb86659fd93ULL);
}

void HpackEncoder::addToDynamicIndex(std::string_view name, std::string_view value) {
  ++_newestDynamicSerial;
  indexDynamicHeader(name, value, _newestDynamicSerial);

  // FIFO eviction leaves old hashes behind. Rebuild after stale entries double the live index rather than paying
  // for two erases on every insertion. The hashes contain no views into Header storage, so stale slots are safe.
  resizeDynamicIndex();
}

void HpackEncoder::resizeDynamicIndex() {
  // The table may hold fewer than kDynamicIndexMinEntries entries once the index is built: a large field evicts many of
  // them. The index is kept, rather than dropped and built again each time the number of entries crosses the threshold.
  if ((_dynamicTable.entryCount() * 4U) + 32U < _dynamicIndex.size()) {
    rebuildDynamicIndex();
  }
}

HpackLookupResult HpackEncoder::findDynamicHeaderIndexed(std::string_view name, std::string_view value) const {
  const auto nameHash = CityHash{}(name);
  if (const auto iter = _dynamicIndex.find(dynamicFullHash(nameHash, value)); iter != _dynamicIndex.end()) {
    const auto tableIdx = dynamicLiveIndex(iter->second);
    if (tableIdx < _dynamicTable.entryCount()) {
      const auto& entry = _dynamicTable[static_cast<uint32_t>(tableIdx)];
      if (entry.name() == name && entry.value() == value) {
        return {HpackLookupResult::Match::Full, HpackDynamicTableIndex(tableIdx)};
      }
    }
    // A disagreement means either a stale serial or a hash collision. Fall back so collisions cannot alter output.
    return FindDynamicHeaderLinear(_dynamicTable, name, value);
  }

  if (const auto iter = _dynamicIndex.find(dynamicNameHash(nameHash)); iter != _dynamicIndex.end()) {
    const auto tableIdx = dynamicLiveIndex(iter->second);
    if (tableIdx < _dynamicTable.entryCount() && _dynamicTable[static_cast<uint32_t>(tableIdx)].name() == name) {
      return {HpackLookupResult::Match::NameOnly, HpackDynamicTableIndex(tableIdx)};
    }
    return FindDynamicHeaderLinear(_dynamicTable, name, value);
  }
  return {};
}

std::size_t HpackEncoder::dynamicLiveIndex(uint64_t serial) const noexcept {
  assert(serial <= _newestDynamicSerial);
  return static_cast<std::size_t>(_newestDynamicSerial - serial);
}

void HpackEncoder::indexDynamicHeader(std::string_view name, std::string_view value, uint64_t serial) {
  const auto nameHash = CityHash{}(name);
  _dynamicIndex[dynamicNameHash(nameHash)] = serial;
  _dynamicIndex[dynamicFullHash(nameHash, value)] = serial;
}

void HpackEncoder::rebuildDynamicIndex() {
  _dynamicIndex.clear();
  _dynamicIndex.reserve(_dynamicTable.entryCount() * 2U);

  for (std::size_t idx = _dynamicTable.entryCount(); idx != 0U; --idx) {
    const auto tableIdx = idx - 1U;
    const auto& entry = _dynamicTable[static_cast<uint32_t>(tableIdx)];

    indexDynamicHeader(entry.name(), entry.value(), _newestDynamicSerial - tableIdx);
  }
}

void HpackEncoder::resetDynamicIndex() {
  _dynamicIndex = {};
  _newestDynamicSerial = 0U;
}

namespace {

// Number of bytes of an integer representation (RFC 7541 §5.1).
constexpr std::size_t IntegerSize(uint64_t value, uint8_t prefixBits) noexcept {
  const uint64_t maxPrefix = (uint64_t{1} << prefixBits) - 1U;
  if (value < maxPrefix) {
    return 1U;
  }
  value -= maxPrefix;
  // 1 byte per 7 bits, minimum 1 byte
  return 1U + (value < 128U ? 1U : (static_cast<std::size_t>(std::bit_width(value)) + 6U) / 7);
}

// Write an integer representation (RFC 7541 §5.1) at pData, which has room for the IntegerSize() bytes of it.
// Returns the end of the integer.
std::byte* WriteInteger(std::byte* pData, uint64_t value, uint8_t prefixBits, uint8_t prefixMask) noexcept {
  const uint8_t maxPrefix = static_cast<uint8_t>((1U << prefixBits) - 1U);

  if (value < maxPrefix) {
    *pData++ = static_cast<std::byte>(prefixMask | static_cast<uint8_t>(value));
    return pData;
  }

  *pData++ = static_cast<std::byte>(prefixMask | maxPrefix);
  for (value -= maxPrefix; value >= 128U; value >>= 7U) {
    *pData++ = static_cast<std::byte>((value & 0x7FU) | 0x80U);
  }
  *pData++ = static_cast<std::byte>(value);
  return pData;
}

void EncodeInteger(RawBytes& output, uint64_t value, uint8_t prefixBits, uint8_t prefixMask) {
  const uint8_t maxPrefix = static_cast<uint8_t>((1U << prefixBits) - 1U);

  if (value < maxPrefix) {
    output.push_back(static_cast<std::byte>(prefixMask | static_cast<uint8_t>(value)));
    return;
  }

  output.ensureAvailableCapacityExponential(IntegerSize(value, prefixBits));
  output.setEnd(WriteInteger(output.data() + output.size(), value, prefixBits, prefixMask));
}

/// Write the Huffman encoding (RFC 7541 §5.2) of 'str' at 'out' if it is shorter than 'str'.
/// 'out' has room for 8 bytes more than the size of 'str'.
/// Returns the size of the Huffman string, 0 if it is not shorter than 'str' ('out' then holds no usable data).
std::size_t EncodeHuffmanIfShorter(std::string_view str, std::byte* out) noexcept {
  // Number of symbols encoded at once, and the number of bits of their codes up to which they are: with the 7 pending
  // bits at most, they fit in 64 bits.
  static constexpr std::ptrdiff_t kNbSymbolsAtOnce = 4;
  static constexpr uint32_t kMaxBitsAtOnce = 56;

  const char* pStr = str.data();
  const char* const pStrEnd = pStr + str.size();
  std::byte* pOut = out;
  // The Huffman string is not shorter once its whole bytes reach this limit.
  const std::byte* const pOutLimit = out + str.size();

  // The bits of the codes, the nbPendingBits last ones (0 to 7 between two writes) being not written yet.
  uint64_t bits = 0;
  uint32_t nbPendingBits = 0;

  // Write the pending bits, at least kHuffmanMinBits of them. 8 bytes are always written, of which only the whole bytes
  // of pending bits are kept: there is no branch depending on the lengths of the codes.
  // Returns whether the Huffman string may still be shorter.
  const auto writePendingBits = [&] {
    Write64BE(pOut, bits << (64U - nbPendingBits));
    pOut += nbPendingBits >> 3U;
    nbPendingBits &= 7U;
    return pOut < pOutLimit;
  };

  // Four symbols at once while their codes are not too long (the ones of header text never are): their bits are
  // gathered apart from the pending ones, which the next symbols only wait for through one shift.
  while (pStrEnd - pStr >= kNbSymbolsAtOnce) {
    const HuffmanCode code1 = kHuffmanCodes[static_cast<uint8_t>(pStr[0])];
    const HuffmanCode code2 = kHuffmanCodes[static_cast<uint8_t>(pStr[1])];
    const HuffmanCode code3 = kHuffmanCodes[static_cast<uint8_t>(pStr[2])];
    const HuffmanCode code4 = kHuffmanCodes[static_cast<uint8_t>(pStr[3])];
    const uint32_t nbBits12 = uint32_t{code1.bitLength} + code2.bitLength;
    const uint32_t nbBits34 = uint32_t{code3.bitLength} + code4.bitLength;
    const uint32_t nbBits = nbBits12 + nbBits34;
    if (nbBits > kMaxBitsAtOnce) [[unlikely]] {
      break;
    }
    pStr += kNbSymbolsAtOnce;
    const uint64_t bits12 = (uint64_t{code1.code} << code2.bitLength) | code2.code;
    const uint64_t bits34 = (uint64_t{code3.code} << code4.bitLength) | code4.code;
    bits = (bits << nbBits) | (bits12 << nbBits34) | bits34;
    nbPendingBits += nbBits;
    if (!writePendingBits()) [[unlikely]] {
      return 0;
    }
  }

  // The last symbols, and the ones of a string holding long codes
  for (; pStr != pStrEnd; ++pStr) {
    const HuffmanCode code = kHuffmanCodes[static_cast<uint8_t>(*pStr)];
    bits = (bits << code.bitLength) | code.code;
    nbPendingBits += code.bitLength;
    if (!writePendingBits()) [[unlikely]] {
      return 0;
    }
  }

  if (nbPendingBits != 0) {
    // Pad with the beginning of EOS (all 1s)
    *pOut++ = static_cast<std::byte>((bits << (8U - nbPendingBits)) | (0xFFU >> nbPendingBits));
    if (pOut == pOutLimit) {
      return 0;
    }
  }
  return static_cast<std::size_t>(pOut - out);
}

// Encode a string literal (RFC 7541 §5.2): as a Huffman string if it is shorter, as a raw one otherwise.
void EncodeString(RawBytes& output, std::string_view str) {
  // The length of a Huffman string is only known once encoded: it is encoded after the room of its longest possible
  // length prefix, the one of the raw string.
  const std::size_t rawPrefixSize = IntegerSize(str.size(), 7);
  output.ensureAvailableCapacityExponential(rawPrefixSize + str.size() + sizeof(uint64_t));

  std::byte* const pPrefix = output.data() + output.size();
  std::byte* pString = pPrefix + rawPrefixSize;

  const std::size_t huffmanSize = EncodeHuffmanIfShorter(str, pString);
  if (huffmanSize != 0) {
    std::byte* const pHuffman = WriteInteger(pPrefix, huffmanSize, 7, 0x80);
    if (pHuffman != pString) {
      // A shorter length prefix than the one of the raw string, whose size is 127 bytes at least
      std::memmove(pHuffman, pString, huffmanSize);
    }
    output.setEnd(pHuffman + huffmanSize);
  } else {
    pString = WriteInteger(pPrefix, str.size(), 7, 0x00);
    Copy(str, reinterpret_cast<char*>(pString));
    output.setEnd(pString + str.size());
  }
}

void EncodeNameValue(RawBytes& output, std::string_view name, std::string_view value, HpackLookupResult lookup,
                     uint8_t prefixBits, uint8_t prefixMask) {
  if (lookup.match == HpackLookupResult::Match::NameOnly) {
    EncodeInteger(output, lookup.index, prefixBits, prefixMask);
  } else {
    output.push_back(static_cast<std::byte>(prefixMask));
    EncodeString(output, name);
  }
  EncodeString(output, value);
}

}  // namespace

HpackEncoder::IndexingMode HpackEncoder::IndexingModeOf(std::string_view name, std::string_view value) noexcept {
  // Credentials are never indexed: a field in the dynamic table is encoded in one byte, so an attacker making a client
  // send guesses of a credential along with it (CRIME) would recognize the right guess by its compressed size. Like
  // nghttp2 and Firefox, long cookie values, hard to guess and sent with each request, are indexed.
  // The other fields are those nghttp2 does not index (but :path, which the client encodes itself), plus
  // last-modified: one value per served file, like etag. All these names are in the static table, so they are encoded
  // as literals with an indexed name. Most names do not have the length of any of them.
  static constexpr std::size_t kMinIndexedCookieSize = 20;
  switch (name.size()) {
    case http::Age.size():
      return name == http::Age ? IndexingMode::WithoutIndexing : IndexingMode::Indexed;
    case http::ETag.size():
      return name == http::ETag ? IndexingMode::WithoutIndexing : IndexingMode::Indexed;
    case http::Cookie.size():
      return name == http::Cookie && value.size() < kMinIndexedCookieSize ? IndexingMode::NeverIndexed
                                                                          : IndexingMode::Indexed;
    case http::Location.size():
      return name == http::Location ? IndexingMode::WithoutIndexing : IndexingMode::Indexed;
    case http::SetCookie.size():
      return name == http::SetCookie ? IndexingMode::WithoutIndexing : IndexingMode::Indexed;
    case http::Authorization.size():  // also the size of last-modified and if-none-match
      if (name == http::Authorization) {
        return IndexingMode::NeverIndexed;
      }
      return name == http::LastModified || name == http::IfNoneMatch ? IndexingMode::WithoutIndexing
                                                                     : IndexingMode::Indexed;
    case http::ContentLength.size():
      return name == http::ContentLength ? IndexingMode::WithoutIndexing : IndexingMode::Indexed;
    case http::IfModifiedSince.size():
      return name == http::IfModifiedSince ? IndexingMode::WithoutIndexing : IndexingMode::Indexed;
    case http::ProxyAuthorization.size():
      return name == http::ProxyAuthorization ? IndexingMode::NeverIndexed : IndexingMode::Indexed;
    default:
      return IndexingMode::Indexed;
  }
}

uint64_t HpackEncoder::encode(RawBytes& output, std::string_view name, std::string_view value, IndexingMode mode) {
  if (hasPendingTableSizeUpdate()) {
    // The smallest size first: the decoder may already have evicted down to it, so this table has to as well, even if
    // the size was raised again since (RFC 7541 §4.2).
    if (_smallestPendingTableSize != _lastPendingTableSize) {
      encodeDynamicTableSizeUpdate(output, _smallestPendingTableSize);
    }
    encodeDynamicTableSizeUpdate(output, _lastPendingTableSize);
    _smallestPendingTableSize = kNoPendingTableSize;
    _lastPendingTableSize = 0;
  }

  // Compute the size of the header field as it would be represented in the HPACK dynamic table (32 bytes of overhead)
  const auto headerFieldSize = name.size() + value.size() + kHpackOverhead;

  // Try to find in tables
  const auto [lookup, fingerprint] = lookUpField(name, value, mode);

  if (lookup.match == HpackLookupResult::Match::Full) {
    // Indexed Header Field (RFC 7541 §6.1)
    // Format: 1xxxxxxx
    EncodeInteger(output, lookup.index, 7, 0x80);
    return headerFieldSize;
  }

  if (mode == IndexingMode::Indexed) {
    // Literal Header Field with Incremental Indexing (RFC 7541 §6.2.1)
    // Format: 01xxxxxx
    EncodeNameValue(output, name, value, lookup, 6, 0x40);

    // Add to dynamic table. Large tables get a lazy encoder-only index once a linear scan becomes expensive.
    const bool added = _dynamicTable.add(name, value, fingerprint);
    _nbAdded += static_cast<uint32_t>(added);
    if (!_dynamicIndex.empty()) {
      if (added) {
        addToDynamicIndex(name, value);
      } else {
        resetDynamicIndex();
      }
    } else if (added && _dynamicTable.entryCount() >= kDynamicIndexMinEntries) {
      _newestDynamicSerial = _dynamicTable.entryCount();
      rebuildDynamicIndex();
    }

  } else if (mode == IndexingMode::NeverIndexed) {
    // Literal Header Field Never Indexed (RFC 7541 §6.2.3)
    // Format: 0001xxxx
    EncodeNameValue(output, name, value, lookup, 4, 0x10);
  } else {
    // Literal Header Field without Indexing (RFC 7541 §6.2.2)
    // Format: 0000xxxx
    EncodeNameValue(output, name, value, lookup, 4, 0x00);
  }

  return headerFieldSize;
}

void HpackEncoder::encodeDynamicTableSizeUpdate(RawBytes& output, uint32_t newSize) {
  // Dynamic Table Size Update (RFC 7541 §6.3). Format: 001xxxxx
  EncodeInteger(output, newSize, 5, 0x20);
  _dynamicTable.setMaxSize(newSize);
  if (!_dynamicIndex.empty()) {
    if (_dynamicTable.entryCount() >= kDynamicIndexMinEntries) {
      resizeDynamicIndex();
    } else {
      resetDynamicIndex();
    }
  }
}

void HpackEncoder::setMaxDynamicTableSize(uint32_t maxSize) {
  if (!hasPendingTableSizeUpdate() && maxSize == _dynamicTable.maxSizeBytes()) {
    return;  // Nothing to signal
  }
  _smallestPendingTableSize = std::min(_smallestPendingTableSize, maxSize);
  _lastPendingTableSize = maxSize;
}

HpackEncoder::FoundMemoSlots HpackEncoder::FoundMemoSlotsOf(std::string_view name, std::string_view value) noexcept {
  const uint32_t firstChar = static_cast<uint8_t>(name.front());
  const uint32_t lastChar = static_cast<uint8_t>(name.back());
  const uint32_t mix = (static_cast<uint32_t>(name.size()) * 0x9E3779B1U) ^
                       (static_cast<uint32_t>(value.size()) * 0x27D4EB2FU) ^ (firstChar * 0x85EBCA6BU) ^
                       (lastChar * 0xC2B2AE35U);
  return {mix >> (32U - kFoundMemoBits), (mix >> (32U - (2U * kFoundMemoBits))) & ((1U << kFoundMemoBits) - 1U)};
}

uint32_t HpackEncoder::foundMemoIndex(uint32_t slot, std::string_view name, std::string_view value) const noexcept {
  const uint32_t idx = _nbAdded - _foundMemo[slot];
  const auto entryCount = static_cast<uint32_t>(_dynamicTable.entryCount());
  if (idx < entryCount) {
    const http::Header& entry = _dynamicTable[idx];
    if (entry.name() == name && entry.value() == value) {
      return idx;
    }
  }
  return entryCount;
}

void HpackEncoder::rememberFound(FoundMemoSlots slots, uint32_t idx) noexcept {
  const bool firstInUse = _nbAdded - _foundMemo[slots.first] < _dynamicTable.entryCount();
  const bool secondInUse = _nbAdded - _foundMemo[slots.second] < _dynamicTable.entryCount();

  _foundMemo[firstInUse && !secondInUse ? slots.second : slots.first] = _nbAdded - idx;
}

HpackEncoder::FieldLookup HpackEncoder::lookUpField(std::string_view name, std::string_view value, IndexingMode mode) {
  assert(!name.empty());  // header name should be already validated.
  assert(std::ranges::none_of(name, [](char ch) { return ch >= 'A' && ch <= 'Z'; }));

  HpackLookupResult result;

  // Search static table first. The hash is perfect over the static names, so a single slot and a single comparison
  // settle it - no probe sequence.
  if (name.size() >= kStaticHeaderNameMinLen && name.size() <= kStaticHeaderNameMaxLen) {
    const auto& hashEntry = kStaticNameHashTable.slots[StaticNameSlot(name)];
    if (hashEntry.name == name) {
      for (uint8_t hashIdx = 0; hashIdx < hashEntry.count; ++hashIdx) {
        const auto entryIdx = kStaticTableByName[hashEntry.sortedStart + hashIdx].index;
        const auto& entry = kStaticTable[entryIdx];
        if (result.match == HpackLookupResult::Match::None) {
          result.index = 1U + entryIdx;  // RFC 7541 is 1-based
          result.match = HpackLookupResult::Match::NameOnly;
        }
        if (entry.value == value) {
          result.index = 1U + entryIdx;
          result.match = HpackLookupResult::Match::Full;
          return {result, 0};
        }
      }
    }
  }

  // A field that is not indexed, with a name of the static table, is encoded with that name index: the dynamic table,
  // filled by this encoder only, holds it only if an earlier field of that name was indexed, which is rare enough (a
  // global header named like it) not to scan the whole table for each of them.
  if (mode != IndexingMode::Indexed && result.match == HpackLookupResult::Match::NameOnly) {
    return {result, 0};
  }

  const auto entryCount = static_cast<uint32_t>(_dynamicTable.entryCount());
  uint32_t idx = 0;
  // Small dynamic tables are faster as a contiguous linear scan comparing the strings, their sizes first. Its newest
  // entries first: all of them on a new connection, and the field values that just changed (the date of the second).
  if (_dynamicIndex.empty()) {
    for (const uint32_t endIdx = std::min(entryCount, kNbNewestEntriesScannedFirst); idx < endIdx; ++idx) {
      const http::Header& entry = _dynamicTable[idx];
      if (entry.name() == name) {
        if (entry.value() == value) {
          return {{HpackLookupResult::Match::Full, HpackDynamicTableIndex(idx)}, 0};
        }
        if (result.match == HpackLookupResult::Match::None) {
          result = {HpackLookupResult::Match::NameOnly, HpackDynamicTableIndex(idx)};
        }
      }
    }
    if (idx == entryCount) {
      return {result, FieldFingerprint(name, value)};
    }
  }

  // Then the entry where this field was last found, the messages of a connection repeating their fields, whatever the
  // number of entries added since. The table never holds two entries with the same name and value (a field found is
  // not added), so that this entry is the one the lookups below would find.
  const FoundMemoSlots memoSlots = FoundMemoSlotsOf(name, value);
  for (const uint32_t slot : {memoSlots.first, memoSlots.second}) {
    if (const uint32_t memoIdx = foundMemoIndex(slot, name, value); memoIdx != entryCount) {
      return {{HpackLookupResult::Match::Full, HpackDynamicTableIndex(memoIdx)}, 0};
    }
  }

  return lookUpOlderEntries(name, value, result);
}

HpackEncoder::FieldLookup HpackEncoder::lookUpOlderEntries(std::string_view name, std::string_view value,
                                                           HpackLookupResult result) {
  const std::size_t fingerprint = FieldFingerprint(name, value);
  const FoundMemoSlots memoSlots = FoundMemoSlotsOf(name, value);
  if (!_dynamicIndex.empty()) {
    const auto indexed = findDynamicHeaderIndexed(name, value);
    if (indexed.match == HpackLookupResult::Match::Full) {
      rememberFound(memoSlots, indexed.index - HpackDynamicTableIndex(0));
      return {indexed, fingerprint};
    }
    return {result.match == HpackLookupResult::Match::None ? indexed : result, fingerprint};
  }

  // Compare the strings of an entry only if its tag, the fingerprint of its field, may be the one of this field: while
  // no entry of the table has this name, the entries whose tag has the half of this name, then the entries whose tag is
  // the fingerprint of this field.
  // A name match keeps the first index found, the smallest one: a name of the static table keeps its index, which takes
  // fewer bytes than the ones of the dynamic table from its second entry on (63 and more do not fit in a 6-bit prefix).
  // The newest entries were scanned already.
  const auto entryCount = static_cast<uint32_t>(_dynamicTable.entryCount());
  for (uint32_t idx = std::min(entryCount, kNbNewestEntriesScannedFirst);; ++idx) {
    idx =
        _dynamicTable.findTag(idx, entryCount, fingerprint,
                              result.match == HpackLookupResult::Match::None ? kNameFingerprintMask : ~std::size_t{0});
    if (idx == entryCount) {
      return {result, fingerprint};
    }
    const http::Header& entry = _dynamicTable[idx];
    if (_dynamicTable.tag(idx) == fingerprint && entry.name() == name && entry.value() == value) {
      rememberFound(memoSlots, idx);
      return {{HpackLookupResult::Match::Full, HpackDynamicTableIndex(idx)}, fingerprint};
    }
    if (result.match == HpackLookupResult::Match::None && entry.name() == name) {
      result = {HpackLookupResult::Match::NameOnly, HpackDynamicTableIndex(idx)};
    }
  }
}

}  // namespace aeronet::http2
