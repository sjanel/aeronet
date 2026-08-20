#include "aeronet/hpack.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <numeric>
#include <span>
#include <string_view>
#include <utility>

#include "aeronet/http-constants.hpp"
#include "aeronet/http-header.hpp"
#include "aeronet/memory-utils-sv.hpp"
#include "aeronet/mergeable-headers.hpp"
#include "aeronet/raw-bytes.hpp"
#include "aeronet/safe-cast.hpp"
#include "http2-header-is-valid.hpp"

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

constexpr uint32_t kStaticNameHashBits = 7;
constexpr uint32_t kStaticNameHashSize = 1UL << kStaticNameHashBits;
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
constexpr HuffmanCode kHuffmanCodes[] = {
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
// Optimized Huffman Decode Tables
// ============================
// Decoding works on a 64-bit MSB-aligned bit window and has two tiers:
// - Tier 1: a direct-mapped table indexed by the top 8 bits. Covers every code of 5..8 bits, which is 74 of the 257
//   symbols but well over 99% of the bytes in real header text (all lowercase letters, digits and common
//   punctuation). One load, no branching on bit length.
// - Tier 2: canonical decoding for the 9..30 bit codes, see kHuffmanLevels below.
//
// 8 bits is the exact useful width: RFC 7541 has no 9-bit code, so a 9-bit table would cover the same symbols with
// twice the footprint. 256 entries keep the table at 1 KB, which matters because a busy server touches it from every
// connection.

// Tier 1 table entry.
struct HuffmanDecodeEntry {
  uint16_t symbol;   // Decoded symbol (0-255); meaningless when bitsUsed == 0
  uint8_t bitsUsed;  // Number of bits consumed, or 0 when the code is longer than the tier-1 window
};

constexpr std::size_t kHuffmanLevel1Bits = 8;
constexpr std::size_t kHuffmanLevel1Size = 1ULL << kHuffmanLevel1Bits;

constexpr auto kHuffmanDecodeTable = [] {
  std::array<HuffmanDecodeEntry, kHuffmanLevel1Size> table;

  // Entries left at bitsUsed == 0 mean "code is longer than the window, fall through to tier 2".
  std::ranges::fill(table, HuffmanDecodeEntry{0, 0});

  for (std::size_t sym = 0; sym < std::size(kHuffmanCodes); ++sym) {
    const auto [code, bitLen] = kHuffmanCodes[sym];
    if (bitLen > kHuffmanLevel1Bits) {
      continue;
    }
    // The window is left-aligned, so one code owns every index sharing its top bitLen bits.
    const std::size_t shift = kHuffmanLevel1Bits - bitLen;
    const std::size_t baseIdx = static_cast<std::size_t>(code) << shift;
    for (std::size_t iter = 0, numEntries = 1ULL << shift; iter < numEntries; ++iter) {
      table[baseIdx + iter] = {static_cast<uint16_t>(sym), bitLen};
    }
  }

  return table;
}();

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

/// Decode one symbol of 9..30 bits from an MSB-aligned bit window.
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

}  // namespace

std::span<const http::HeaderView> GetHpackStaticTable() noexcept { return kStaticTable; }

// ============================
// HpackDynamicTable
// ============================

bool HpackDynamicTable::add(std::string_view name, std::string_view value) {
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
        // reuse memory from the evicted entry to avoid a new malloc/free pair
        newEntry = http::Header(std::move(evicted), name, value);
      }
    } while (_maxSizeBytes < _currentSizeBytes + entrySize);

    // Add the entry after the eviction(s).
    _entries.push_back(std::move(newEntry));
  } else {
    // Fast path (no eviction) - construct then noexcept-move into the vector.
    // Using push_back (noexcept move) instead of emplace_back avoids exception-handling
    // code around the malloc inside the constructor, producing tighter codegen.
    _entries.push_back(http::Header(name, value));
  }

  _currentSizeBytes += entrySize;

  return true;
}

void HpackDynamicTable::setMaxSize(std::size_t maxSize) {
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
  http::Header evictedEntry = std::move(_entries[_firstLive]);
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
  static constexpr uint64_t kInvalidIndex = std::numeric_limits<uint64_t>::max();

  uint64_t index{kInvalidIndex};
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
    ret.index = value;
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
    value += (currByte & 0x7F) * multiplier;

    // Overflow cannot happen, we can only multiply by 128 (2⁷) kMaxContinuationBytes times.
    static_assert(kMaxContinuationBytes * 7 < static_cast<uint8_t>(sizeof(uint64_t) * 8),
                  "Multiplier overflow possible in DecodeInteger");

    multiplier *= 128;
    ++pos;

    if ((currByte & 0x80) == 0) {
      ret.index = value;
      ret.consumed = pos;
      break;
    }
  }

  return ret;
}

}  // namespace

HpackDecoder::HpackDecoder(std::size_t maxDynamicTableSize, bool mergeAllowedForUnknownRequestHeaders)
    : _dynamicTable(maxDynamicTableSize),
      _decodedStrings(128U),
      _mergeAllowedForUnknownRequestHeaders(mergeAllowedForUnknownRequestHeaders) {}

HpackDecoder::DecodeResult HpackDecoder::decode(std::span<const std::byte> data) {
  _decodedStrings.clear();
  _decodedHeadersMap.clear();

  std::size_t pos = 0;

  DecodeResult res{_decodedHeadersMap, 0UL};

  auto recordFieldError = [&res](DecodeResult::Error err) {
    if (res.error == DecodeResult::Error::None) {
      res.error = err;
    }
  };

  bool seenRegularHeader = false;

  while (pos < data.size()) {
    const uint8_t firstByte = static_cast<uint8_t>(data[pos]);

    if ((firstByte & 0x80) != 0) {
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

      http::HeaderView header = lookupIndex(indexResult.index);
      if (header.name.empty()) {
        res.error = DecodeResult::Error::IndexOutOfBoundsInIndexedHeaderField;
        return res;
      }

      res.headerListSize += header.name.size() + header.value.size() + kHpackOverhead;
      recordFieldError(storeHeader(header.name, header.value, seenRegularHeader));

    } else if ((firstByte & 0xE0) == 0x20) {
      // Dynamic Table Size Update (RFC 7541 §6.3) - Format: 001xxxxx
      const auto sizeResult = DecodeInteger(data.subspan(pos), 5);
      if (sizeResult.index == DecodedIndex::kInvalidIndex) {
        res.error = DecodeResult::Error::FailedToDecodeDynamicTableSizeUpdate;
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
        res.error = DecodeResult::Error::FailedToDecodeLiteralHeaderIndex;
        return res;
      }
      pos += indexResult.consumed;

      std::string_view name;
      if (indexResult.index == 0) {
        // New literal name
        const auto nameResult = decodeString(data.subspan(pos));
        if (nameResult.consumed == DecodedString::kInvalidConsumed) {
          res.error = DecodeResult::Error::FailedToDecodeLiteralHeaderName;
          return res;
        }
        pos += nameResult.consumed;
        name = nameResult.str;
      } else {
        // Indexed name
        const auto header = lookupIndex(indexResult.index);
        if (header.name.empty()) {
          res.error = DecodeResult::Error::IndexOutOfBoundsForHeaderName;
          return res;
        }
        name = header.name;
      }

      const auto valueResult = decodeString(data.subspan(pos));
      if (valueResult.consumed == DecodedString::kInvalidConsumed) {
        res.error = DecodeResult::Error::FailedToDecodeLiteralHeaderValue;
        return res;
      }
      pos += valueResult.consumed;

      res.headerListSize += name.size() + valueResult.str.size() + kHpackOverhead;
      recordFieldError(storeHeader(name, valueResult.str, seenRegularHeader));

      if (withIndexing) {
        // Note: add() copies name/value before evicting, so this is safe even if name/value point to data owned by
        // entries that will be evicted. Dynamic table must be updated even in case of header validation issue - cf. RFC
        // 9113 §4.3.
        _dynamicTable.add(name, valueResult.str);
      }
    }
  }

  return res;
}

HpackDecoder::DecodedString HpackDecoder::decodeString(std::span<const std::byte> data) {
  const auto [length, consumed] = DecodeInteger(data, 7);
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

  const auto stringData = data.subspan(consumed, length);

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
  // Max decoded length for N input bytes: at most floor(8*N/5) symbols.
  const auto maxLen = SafeCast<CharStorageSizeType>((data.size() * 8U) / 5);
  char* buf = _decodedStrings.allocateAndDefaultConstruct(maxLen);
  std::size_t sz = 0;

  // Bit buffer: accumulates bits for decoding (MSB-aligned)
  // Bits are stored left-aligned: new bytes go at position (64 - 8 - bitsInBuffer)
  uint64_t bitBuffer = 0;
  int32_t bitsInBuffer = 0;
  std::size_t byteIdx = 0;

  while (true) {
    // Refill bit buffer - pack bytes from MSB side.
    // Stopping at 55 caps bitsInBuffer at 63, which keeps the padding shift below the width of the type while still
    // guaranteeing the 30 bits the longest code needs.
    while (bitsInBuffer <= 55 && byteIdx < data.size()) {
      bitBuffer |= static_cast<uint64_t>(static_cast<uint8_t>(data[byteIdx])) << (56 - bitsInBuffer);
      bitsInBuffer += 8;
      ++byteIdx;
    }

    if (bitsInBuffer == 0) {
      break;
    }
    assert(bitsInBuffer > 0 && bitsInBuffer < 64);

    // Set every bit past the real ones, so a lookup never depends on how many bits are left. A valid stream is padded
    // with the EOS prefix (all 1s), so this reproduces exactly what the encoder wrote and lets the tier-1 table serve
    // the final symbol of the string. Without it the tail of *every* string would fall through to the slow path.
    const uint64_t window = bitBuffer | (~uint64_t{0} >> bitsInBuffer);

    const auto& entry = kHuffmanDecodeTable[static_cast<std::size_t>(window >> (64U - kHuffmanLevel1Bits))];
    uint16_t symbol = entry.symbol;
    int32_t bitLength = entry.bitsUsed;

    if (bitLength == 0) {
      // Code is longer than the tier-1 window: decode canonically.
      const auto canonical = DecodeHuffmanCanonical(window);
      symbol = canonical.symbol;
      bitLength = canonical.bitLength;
    }

    // Must come before the EOS test: trailing padding is all 1s, which is the EOS prefix, so a tail of padding always
    // "decodes" to EOS. Only a code that fits in the bits actually present is a real symbol.
    if (bitLength > bitsInBuffer) {
      break;  // What is left is padding; validated below.
    }

    if (symbol == 256) [[unlikely]] {
      _decodedStrings.shrinkLastAllocated(buf, 0);
      return {};  // EOS must not appear in the data (RFC 7541 §5.2)
    }

    assert(symbol < 256);
    assert(sz < maxLen);
    buf[sz++] = static_cast<char>(symbol);

    bitBuffer <<= bitLength;
    bitsInBuffer -= bitLength;
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

http::HeaderView HpackDecoder::lookupIndex(uint64_t index) const {
  // Static table: indices 1-61
  http::HeaderView ret;
  if (index <= std::size(kStaticTable)) {
    const auto& entry = kStaticTable[index - 1];
    ret.name = entry.name;
    ret.value = entry.value;
    return ret;
  }

  // Dynamic table: indices 62+
  const uint32_t dynamicIndex = static_cast<uint32_t>(index - std::size(kStaticTable) - 1);
  if (dynamicIndex >= _dynamicTable.entryCount()) {
    return ret;
  }
  const auto& entry = _dynamicTable[dynamicIndex];
  ret.name = entry.name();
  ret.value = entry.value();
  return ret;
}

HpackDecoder::DecodeResult::Error HpackDecoder::storeHeader(std::string_view name, std::string_view value,
                                                            bool& seenRegularHeader) {
  if (!IsValidHTTP2HeaderName(name)) {
    return DecodeResult::Error::MalformedFieldName;
  }

  if (name.front() == ':') {  // pseudo header
    if (seenRegularHeader) {
      return DecodeResult::Error::PseudoHeaderFieldAfterRegularField;
    }
    if (name != http::PseudoHeaderMethod && name != http::PseudoHeaderScheme && name != http::PseudoHeaderAuthority &&
        name != http::PseudoHeaderPath && name != http::PseudoHeaderStatus && name != http::PseudoHeaderProtocol) {
      return DecodeResult::Error::UndefinedPseudoHeaderField;
    }
    if (_decodedHeadersMap.contains(name)) {
      return DecodeResult::Error::DuplicatePseudoHeaderField;
    }
  } else {
    seenRegularHeader = true;
  }

  if (!IsValidHTTP2HeaderValue(value)) {
    return DecodeResult::Error::MalformedFieldValue;
  }

  char* pName = _decodedStrings.allocateAndDefaultConstruct(SafeCast<CharStorageSizeType>(name.size() + value.size()));
  Copy(name, pName);

  char* pValue = pName + name.size();
  Copy(value, pValue);

  auto [it, inserted] = _decodedHeadersMap.try_emplace(std::string_view(pName, name.size()), pValue, value.size());

  if (!inserted) {
    // Header already exists
    std::string_view existingValue = it->second;

    // value not needed anymore
    _decodedStrings.shrinkLastAllocated(pName, name.size());

    const char mergeSep = http::ReqHeaderValueSeparator(it->first, _mergeAllowedForUnknownRequestHeaders);
    if (mergeSep == '\0') {
      return DecodeResult::Error::DuplicateHeaderForbiddenToMerge;
    }

    const std::size_t newValueLen = existingValue.size() + 1U + value.size();

    char* pData = _decodedStrings.allocateAndDefaultConstruct(SafeCast<CharStorageSizeType>(newValueLen));

    pData = Append(existingValue, pData);
    *pData++ = mergeSep;
    pData = Append(value, pData);

    it->second = std::string_view(pData - newValueLen, newValueLen);
  }

  return DecodeResult::Error::None;
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

  value -= maxPrefix;

  // Bytes needed to varint-encode `value`: 1 byte per 7 bits, minimum 1 byte
  // (this matches exactly what the while-loop below produces).
  const std::size_t continuationBytes = value < 128 ? 1 : (static_cast<std::size_t>(std::bit_width(value)) + 6) / 7;

  output.ensureAvailableCapacityExponential(1U + continuationBytes);

  std::byte* pData = output.data() + output.size();

  *pData++ = static_cast<std::byte>(prefixMask | maxPrefix);

  while (value >= 128) {
    *pData++ = static_cast<std::byte>((value & 0x7F) | 0x80);
    value >>= 7;
  }
  *pData++ = static_cast<std::byte>(value);

  output.setEnd(pData);
}

std::size_t HuffmanEncodedLength(std::string_view str) noexcept {
  return std::transform_reduce(str.begin(), str.end(), std::size_t{7}, std::plus<>{},
                               [](uint8_t ch) { return kHuffmanCodes[ch].bitLength; }) /
         8;
}

/// Encode 'str' with the RFC 7541 Huffman code. 'encodedLen' must be HuffmanEncodedLength(str): the caller already
/// needs it to decide between Huffman and raw, so it is passed in rather than walking the string a second time.
void EncodeHuffman(RawBytes& output, std::string_view str, std::size_t encodedLen) {
  uint64_t currentCode = 0;
  uint8_t currentBits = 0;

  assert(encodedLen == HuffmanEncodedLength(str));
  output.ensureAvailableCapacityExponential(encodedLen);

  std::byte* pData = output.data() + output.size();

  for (char ch : str) {
    const auto code = kHuffmanCodes[static_cast<uint8_t>(ch)];

    currentCode = (currentCode << code.bitLength) | code.code;
    currentBits += code.bitLength;
    while (currentBits >= 8) {
      currentBits -= 8;
      *pData++ = static_cast<std::byte>((currentCode >> currentBits) & 0xFF);
    }
  }

  // Pad with EOS prefix (all 1s)
  if (currentBits > 0) {
    const uint8_t padding = static_cast<uint8_t>((1U << (8 - currentBits)) - 1);
    *pData++ = static_cast<std::byte>((currentCode << (8 - currentBits)) | padding);
  }
  output.setEnd(pData);
}

void EncodeString(RawBytes& output, std::string_view str) {
  const std::size_t huffmanLen = HuffmanEncodedLength(str);
  if (huffmanLen < str.size()) {
    // Huffman encoding is more efficient
    EncodeInteger(output, huffmanLen, 7, 0x80);
    EncodeHuffman(output, str, huffmanLen);
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
  const auto lookup = findHeader(name, value);

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

  // Search static table first. The hash is perfect over the static names, so a single slot and a single comparison
  // settle it - no probe sequence.
  if (name.size() >= kStaticHeaderNameMinLen && name.size() <= kStaticHeaderNameMaxLen) {
    const auto& hashEntry = kStaticNameHashTable.slots[StaticNameSlot(name)];
    if (hashEntry.name == name) {
      for (uint8_t ii = 0; ii < hashEntry.count; ++ii) {
        const auto entryIdx = kStaticTableByName[hashEntry.sortedStart + ii].index;
        const auto& entry = kStaticTable[entryIdx];
        if (result.match == HpackLookupResult::Match::None) {
          result.index = 1U + entryIdx;  // RFC 7541 is 1-based
          result.match = HpackLookupResult::Match::NameOnly;
        }
        if (entry.value == value) {
          result.index = 1U + entryIdx;
          result.match = HpackLookupResult::Match::Full;
          return result;
        }
      }
    }
  }

  // Search dynamic table in a linear scan, which is reasonable since dynamic table is expected to be small and recently
  // added entries are more likely to match.
  for (uint32_t idx = 0; idx < _dynamicTable.entryCount(); ++idx) {
    const auto& entry = _dynamicTable[idx];
    if (entry.name() == name) {
      result.index = static_cast<uint32_t>(std::size(kStaticTable)) + 1U + idx;
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
