#include "aeronet/websocket-utf8.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>

#include "aeronet/avx2.hpp"
#include "aeronet/vector.hpp"
#include "aeronet/websocket-constants.hpp"

namespace aeronet::websocket {

namespace {

struct Kernel {
  std::string_view name;
  bool (*isValid)(std::span<const std::byte>) noexcept;
  // Its variant unmasking the data in the same pass (none for the scalar kernel).
  bool (*unmaskAndValidate)(std::span<std::byte>, MaskingKey) noexcept;
};

using KernelVector = vector<Kernel>;

// The dispatchers and every kernel the CPU supports.
const KernelVector& Kernels() {
  static const KernelVector kKernels = [] {
    KernelVector kernels{
        {"dispatch", &IsValidUtf8, &UnmaskAndValidateUtf8},
        {"scalar", &detail::IsValidUtf8Scalar, nullptr},
    };
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    if (HasSsse3()) {
      kernels.push_back({"ssse3", &detail::IsValidUtf8Ssse3, &detail::UnmaskAndValidateUtf8Ssse3});
    }
    if (HasAvx2()) {
      kernels.push_back({"avx2", &detail::IsValidUtf8Avx2, &detail::UnmaskAndValidateUtf8Avx2});
    }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    kernels.push_back({"neon", &detail::IsValidUtf8Neon, &detail::UnmaskAndValidateUtf8Neon});
#endif
    return kernels;
  }();
  return kKernels;
}

// Masking key of the data given to the unmasking kernels: distinct bytes, with and without their high bit.
constexpr std::byte kKeyBytes[]{
    std::byte{0xC3},
    std::byte{0x5A},
    std::byte{0x01},
    std::byte{0xF0},
};

MaskingKey TestMaskingKey() {
  MaskingKey maskingKey;
  std::memcpy(&maskingKey, kKeyBytes, kMaskingKeySize);
  return maskingKey;
}

// Independent reference: the well-formed byte sequences of the Unicode Standard (table 3-7).
bool ReferenceIsValidUtf8(std::string_view data) {
  std::size_t pos = 0;
  while (pos < data.size()) {
    const auto lead = static_cast<uint8_t>(data[pos]);
    if (lead < 0x80) {
      ++pos;
      continue;
    }
    std::size_t len = 0;
    uint8_t secondMin = 0x80;
    uint8_t secondMax = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
      len = 2;
    } else if (lead == 0xE0) {
      len = 3;
      secondMin = 0xA0;
    } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
      len = 3;
    } else if (lead == 0xED) {
      len = 3;
      secondMax = 0x9F;
    } else if (lead == 0xF0) {
      len = 4;
      secondMin = 0x90;
    } else if (lead >= 0xF1 && lead <= 0xF3) {
      len = 4;
    } else if (lead == 0xF4) {
      len = 4;
      secondMax = 0x8F;
    } else {
      return false;
    }
    if (data.size() - pos < len) {
      return false;
    }
    const auto second = static_cast<uint8_t>(data[pos + 1]);
    if (second < secondMin || second > secondMax) {
      return false;
    }
    for (std::size_t idx = 2; idx < len; ++idx) {
      if ((static_cast<uint8_t>(data[pos + idx]) & 0xC0U) != 0x80U) {
        return false;
      }
    }
    pos += len;
  }
  return true;
}

// Name of the first kernel whose verdict on data differs from expected, empty if they all agree. The unmasking
// variants get the data masked, and must also leave it entirely unmasked, whatever their verdict.
// The data is copied to allocations of its exact size, for ASAN to report any access past their end.
std::string FirstMismatch(std::string_view data, bool expected) {
  const auto copy = std::make_unique_for_overwrite<char[]>(data.size());
  std::ranges::copy(data, copy.get());
  const auto bytes = std::as_bytes(std::span<const char>(copy.get(), data.size()));
  const auto maskedCopy = std::make_unique_for_overwrite<std::byte[]>(data.size());
  const std::span<std::byte> masked(maskedCopy.get(), data.size());
  for (const Kernel& kernel : Kernels()) {
    if (kernel.isValid(bytes) != expected) {
      return std::string(kernel.name);
    }
    if (kernel.unmaskAndValidate != nullptr) {
      for (std::size_t idx = 0; idx < data.size(); ++idx) {
        masked[idx] = bytes[idx] ^ kKeyBytes[idx % kMaskingKeySize];
      }
      if (kernel.unmaskAndValidate(masked, TestMaskingKey()) != expected) {
        return std::string(kernel.name) + " (unmask)";
      }
      if (!std::ranges::equal(masked, bytes)) {
        return std::string(kernel.name) + " (unmasked data)";
      }
    }
  }
  return {};
}

std::string Hex(std::string_view data) {
  static constexpr std::string_view kDigits = "0123456789ABCDEF";
  std::string hex;
  for (const char ch : data) {
    const auto byte = static_cast<uint8_t>(ch);
    hex += kDigits[byte >> 4U];
    hex += kDigits[byte & 0x0FU];
    hex += ' ';
  }
  return hex;
}

// Checks every kernel against the reference validator.
void ExpectMatchesReference(std::string_view data) {
  const bool expected = ReferenceIsValidUtf8(data);
  EXPECT_EQ(FirstMismatch(data, expected), "")
      << "expected " << expected << " for size " << data.size() << ": " << Hex(data.substr(0, 256));
}

// Valid UTF-8 of exactly 'size' bytes, mixing 4, 3, 2 and 1-byte characters.
std::string MultibyteText(std::size_t size) {
  static constexpr std::string_view kChars[]{"\xF0\x9F\x8C\x8D", "\xE4\xB8\xAD", "\xC3\xA9", "a"};
  std::string text;
  for (std::size_t charPos = 0; text.size() < size; ++charPos) {
    const std::string_view ch = kChars[charPos % std::size(kChars)];
    text += ch.size() <= size - text.size() ? ch : "a";
  }
  return text;
}

constexpr std::string_view kValidSamples[]{
    std::string_view("\0", 1),           // U+0000
    "\x7F",                              // U+007F
    "\xC2\x80",                          // U+0080, smallest 2-byte
    "\xDF\xBF",                          // U+07FF, largest 2-byte
    "\xE0\xA0\x80",                      // U+0800, smallest 3-byte
    "\xE1\x80\x80",                      // U+1000
    "\xEC\xBF\xBF",                      // U+CFFF
    "\xED\x80\x80",                      // U+D000
    "\xED\x9F\xBF",                      // U+D7FF, just below the surrogates
    "\xEE\x80\x80",                      // U+E000, just above the surrogates
    "\xEF\xBF\xBF",                      // U+FFFF, largest 3-byte
    "\xF0\x90\x80\x80",                  // U+10000, smallest 4-byte
    "\xF1\x80\x80\x80",                  // U+40000
    "\xF3\xBF\xBF\xBF",                  // U+FFFFF
    "\xF4\x80\x80\x80",                  // U+100000
    "\xF4\x8F\xBF\xBF",                  // U+10FFFF, largest code point
    "\xC3\xA9t\xC3\xA9",                 // "été"
    "\xE4\xB8\x96\xE7\x95\x8C",          // "世界"
    "\xF0\x9F\x8C\x8D\xF0\x9F\x98\x80",  // two emojis
    "\xEF\xBB\xBF",                      // BOM
    "Hello \xE4\xB8\x96\xE7\x95\x8C \xF0\x9F\x8C\x8D",
    "\xC2\xA9\xE2\x82\xAC\xF0\x9D\x84\x9E",  // 2, 3 and 4-byte characters in a row
};

constexpr std::string_view kInvalidSamples[]{
    "\x80",                      // lone continuation
    "\xBF",                      // lone continuation
    "a\x80",                     // continuation after ASCII
    "\xC0\x80",                  // overlong U+0000
    "\xC1\xBF",                  // overlong U+007F
    "\xC0",                      // never valid
    "\xC1",                      // never valid
    "\xE0\x80\x80",              // overlong U+0000
    "\xE0\x9F\xBF",              // overlong U+07FF
    "\xF0\x80\x80\x80",          // overlong U+0000
    "\xF0\x8F\xBF\xBF",          // overlong U+FFFF
    "\xED\xA0\x80",              // U+D800, first surrogate
    "\xED\xAF\xBF",              // U+DBFF
    "\xED\xB0\x80",              // U+DC00
    "\xED\xBF\xBF",              // U+DFFF, last surrogate
    "\xED\xA0\x80\xED\xB0\x80",  // surrogate pair
    "\xF4\x90\x80\x80",          // U+110000
    "\xF4\xBF\xBF\xBF",          // above U+10FFFF
    "\xF5\x80\x80\x80",          // lead above U+10FFFF
    "\xF7\xBF\xBF\xBF",          // lead above U+10FFFF
    "\xF8\x88\x80\x80\x80",      // 5-byte form
    "\xFC\x84\x80\x80\x80\x80",  // 6-byte form
    "\xFE",                      // never valid
    "\xFF",                      // never valid
    "\xF8",                      // never valid
    "\xC2",                      // truncated 2-byte
    "\xE0\xA0",                  // truncated 3-byte
    "\xE4",                      // truncated 3-byte
    "\xF0\x90\x80",              // truncated 4-byte
    "\xF0\x90",                  // truncated 4-byte
    "\xF4",                      // truncated 4-byte
    "\xC2\x41",                  // 2-byte lead followed by ASCII
    "\xE4\xB8\x41",              // 3-byte lead with one continuation, then ASCII
    "\xF0\x9F\x8C\x41",          // 4-byte lead with two continuations, then ASCII
    "\xC2\xC2\x80",              // lead followed by a lead
    "\xE4\xE4\xB8\xAD",          // lead followed by a lead
    "\xC2\x80\x80",              // 2-byte character with an extra continuation
    "\xE4\xB8\xAD\x80",          // 3-byte character with an extra continuation
    "\xF0\x9F\x8C\x8D\x80",      // 4-byte character with an extra continuation
    "\xE4\x80\xF0\x9F\x8C",      // two truncated sequences
    "\xF0\x9F\x8C\x8D\xBF\xBF",  // 4-byte character followed by two continuations
    "\xF4\x90",                  // too large, truncated
    "\xED\xA0",                  // surrogate, truncated
    "\xE0\x80",                  // overlong, truncated
};

TEST(WebSocketUtf8, Empty) {
  EXPECT_EQ(FirstMismatch({}, true), "");
  // A null pointer with zero size is valid too.
  for (const Kernel& kernel : Kernels()) {
    EXPECT_TRUE(kernel.isValid(std::span<const std::byte>{})) << kernel.name;
    if (kernel.unmaskAndValidate != nullptr) {
      EXPECT_TRUE(kernel.unmaskAndValidate(std::span<std::byte>{}, TestMaskingKey())) << kernel.name;
    }
  }
}

TEST(WebSocketUtf8, AsciiOfEverySize) {
  std::string data;
  for (std::size_t size = 0; size <= 200; ++size) {
    EXPECT_EQ(FirstMismatch(data, true), "") << "size " << size;
    data.push_back(static_cast<char>(size % 128));
  }
}

TEST(WebSocketUtf8, MultibyteTextOfEverySize) {
  for (std::size_t size = 0; size <= 200; ++size) {
    EXPECT_EQ(FirstMismatch(MultibyteText(size), true), "") << "size " << size;
  }
}

TEST(WebSocketUtf8, ReferenceAgreesWithSamples) {
  for (const std::string_view sample : kValidSamples) {
    EXPECT_TRUE(ReferenceIsValidUtf8(sample)) << Hex(sample);
  }
  for (const std::string_view sample : kInvalidSamples) {
    EXPECT_FALSE(ReferenceIsValidUtf8(sample)) << Hex(sample);
  }
}

TEST(WebSocketUtf8, ValidSamples) {
  for (const std::string_view sample : kValidSamples) {
    EXPECT_EQ(FirstMismatch(sample, true), "") << Hex(sample);
  }
}

TEST(WebSocketUtf8, InvalidSamples) {
  for (const std::string_view sample : kInvalidSamples) {
    EXPECT_EQ(FirstMismatch(sample, false), "") << Hex(sample);
  }
}

// Places each sample at every offset of the first SIMD blocks, after and before ASCII or multibyte text, so that it
// straddles each block boundary and lands in the zero-padded last block.
TEST(WebSocketUtf8, SamplesAtEveryOffset) {
  static constexpr std::size_t kSuffixSizes[]{0, 1, 3, 17, 40};
  vector<std::string_view> samples(std::begin(kValidSamples), std::end(kValidSamples));
  samples.insert(samples.end(), std::begin(kInvalidSamples), std::end(kInvalidSamples));
  for (const std::string_view sample : samples) {
    const bool expected = ReferenceIsValidUtf8(sample);
    for (std::size_t prefixSize = 0; prefixSize <= 70; ++prefixSize) {
      for (const std::size_t suffixSize : kSuffixSizes) {
        for (const bool multibyte : {false, true}) {
          std::string data = multibyte ? MultibyteText(prefixSize) : std::string(prefixSize, 'p');
          data += sample;
          data += multibyte ? MultibyteText(suffixSize) : std::string(suffixSize, 's');
          ASSERT_EQ(FirstMismatch(data, expected), "")
              << Hex(sample) << " prefix " << prefixSize << " suffix " << suffixSize << " multibyte " << multibyte;
        }
      }
    }
  }
}

TEST(WebSocketUtf8, SequenceTruncatedAtBlockEnd) {
  // No partial last block: the truncation is only visible from the end of the last full block.
  for (const std::size_t size : {16U, 32U, 64U, 128U}) {
    for (const std::string_view lead : {"\xC3", "\xE4\xB8", "\xE4", "\xF0\x9F\x8C", "\xF0\x9F", "\xF0"}) {
      std::string data(size - lead.size(), 'a');
      data += lead;
      EXPECT_EQ(FirstMismatch(data, false), "") << Hex(lead) << " size " << size;
    }
  }
}

TEST(WebSocketUtf8, SequenceTruncatedBeforeAsciiBlock) {
  // The block after the truncated sequence is pure ASCII, which skips the full check.
  for (const std::size_t blockSize : {16U, 32U}) {
    for (const std::string_view lead : {"\xC3", "\xE4\xB8", "\xF0\x9F\x8C"}) {
      std::string data(blockSize - lead.size(), 'a');
      data += lead;
      data.append(64, 'b');
      EXPECT_EQ(FirstMismatch(data, false), "") << Hex(lead) << " block " << blockSize;
    }
  }
}

TEST(WebSocketUtf8, SequenceInterruptedByAsciiBlock) {
  // The continuation bytes after the ASCII block would complete the sequence, were the ASCII block not checked.
  struct SplitSequence {
    std::string_view lead;
    std::string_view continuations;
  };
  static constexpr SplitSequence kSplitSequences[]{
      {"\xC3", "\xA9"}, {"\xE4\xB8", "\xAD"}, {"\xE4", "\xB8\xAD"}, {"\xF0\x9F\x8C", "\x8D"}, {"\xF0", "\x9F\x8C\x8D"},
  };
  for (const std::size_t blockSize : {16U, 32U}) {
    for (const auto [lead, continuations] : kSplitSequences) {
      std::string data(blockSize - lead.size(), 'a');
      data += lead;
      data.append(32, 'b');
      data += continuations;
      data.append(40, 'c');
      EXPECT_EQ(FirstMismatch(data, false), "") << Hex(lead) << " block " << blockSize;
    }
  }
}

TEST(WebSocketUtf8, ContinuationAfterAsciiBlock) {
  // A complete multibyte character ends a block, an ASCII block follows, then a stray continuation byte.
  std::string data(29, 'a');
  data += "\xE4\xB8\xAD";
  data.append(32, 'b');
  data += '\x80';
  data.append(40, 'c');
  EXPECT_EQ(FirstMismatch(data, false), "");
}

// Every pair of bytes, at offsets around the 16 and 32-byte block boundaries and in the last block.
TEST(WebSocketUtf8, EveryBytePair) {
  for (const std::size_t offset : {0U, 15U, 31U, 37U}) {
    std::string data(offset + 2 + 8, 'a');
    for (unsigned first = 0; first < 256; ++first) {
      for (unsigned second = 0; second < 256; ++second) {
        data[offset] = static_cast<char>(first);
        data[offset + 1] = static_cast<char>(second);
        const bool expected = ReferenceIsValidUtf8(data);
        ASSERT_EQ(FirstMismatch(data, expected), "") << Hex(data.substr(offset, 2)) << " at " << offset;
      }
    }
  }
}

// Every lead byte followed by 3 bytes from the values on either side of each decision boundary, straddling a block
// boundary.
TEST(WebSocketUtf8, EveryLeadWithBoundaryContinuations) {
  static constexpr uint8_t kNextBytes[]{
      0x00, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0, 0xC2, 0xE0, 0xF0, 0xFF,
  };
  for (const std::size_t offset : {14U, 30U}) {
    std::string data(offset + 4 + 5, 'a');
    for (unsigned lead = 0x80; lead < 256; ++lead) {
      data[offset] = static_cast<char>(lead);
      for (const uint8_t byte1 : kNextBytes) {
        data[offset + 1] = static_cast<char>(byte1);
        for (const uint8_t byte2 : kNextBytes) {
          data[offset + 2] = static_cast<char>(byte2);
          for (const uint8_t byte3 : kNextBytes) {
            data[offset + 3] = static_cast<char>(byte3);
            const bool expected = ReferenceIsValidUtf8(data);
            ASSERT_EQ(FirstMismatch(data, expected), "") << Hex(data.substr(offset, 4)) << " at " << offset;
          }
        }
      }
    }
  }
}

// Valid text of random characters and sizes, then corrupted by random byte changes, insertions and deletions.
TEST(WebSocketUtf8, RandomMutations) {
  std::mt19937 rng(42);  // NOLINT(bugprone-random-generator-seed) - reproducible
  const auto randomBelow = [&rng](std::size_t bound) {
    return std::uniform_int_distribution<std::size_t>(0, bound - 1)(rng);
  };
  const auto appendCodePoint = [](std::string& out, uint32_t codePoint) {
    if (codePoint < 0x80U) {
      out += static_cast<char>(codePoint);
    } else if (codePoint < 0x800U) {
      out += static_cast<char>(0xC0U | (codePoint >> 6U));
      out += static_cast<char>(0x80U | (codePoint & 0x3FU));
    } else if (codePoint < 0x10000U) {
      out += static_cast<char>(0xE0U | (codePoint >> 12U));
      out += static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU));
      out += static_cast<char>(0x80U | (codePoint & 0x3FU));
    } else {
      out += static_cast<char>(0xF0U | (codePoint >> 18U));
      out += static_cast<char>(0x80U | ((codePoint >> 12U) & 0x3FU));
      out += static_cast<char>(0x80U | ((codePoint >> 6U) & 0x3FU));
      out += static_cast<char>(0x80U | (codePoint & 0x3FU));
    }
  };
  // The code points of each encoded size (surrogates excluded).
  struct CodePointRange {
    uint32_t begin;
    uint32_t end;
  };
  static constexpr CodePointRange kRanges[]{
      {0, 0x80}, {0x80, 0x800}, {0x800, 0xD800}, {0xE000, 0x10000}, {0x10000, 0x110000},
  };

  for (int iteration = 0; iteration < 20000; ++iteration) {
    std::string data;
    const std::size_t nbChars = randomBelow(160);
    for (std::size_t charPos = 0; charPos < nbChars; ++charPos) {
      // Pick the encoded size first, so that each size is frequent.
      const CodePointRange range = kRanges[randomBelow(std::size(kRanges))];
      appendCodePoint(data, range.begin + static_cast<uint32_t>(randomBelow(range.end - range.begin)));
    }
    ASSERT_TRUE(ReferenceIsValidUtf8(data));
    ExpectMatchesReference(data);

    for (std::size_t mutation = randomBelow(4); mutation > 0; --mutation) {
      const std::size_t pos = randomBelow(data.size() + 1);
      const auto byte = static_cast<char>(randomBelow(256));
      switch (randomBelow(3)) {
        case 0:
          if (pos < data.size()) {
            data[pos] = byte;
          }
          break;
        case 1:
          data.insert(pos, 1, byte);
          break;
        default:
          if (pos < data.size()) {
            data.erase(pos, 1);
          }
          break;
      }
    }
    ExpectMatchesReference(data);
    if (HasFailure()) {
      return;
    }
  }
}

TEST(WebSocketUtf8, LargePayload) {
  std::string data;
  while (data.size() < (1U << 20U)) {
    data.append(1000, 'x');
    data += MultibyteText(997);
    data += "\xF0\x9F\x8C\x8D\xE4\xB8\xAD\xC3\xA9";
  }
  EXPECT_EQ(FirstMismatch(data, true), "");

  for (const std::size_t pos : {std::size_t{0}, std::size_t{1000}, data.size() / 2, data.size() - 9, data.size() - 1}) {
    std::string corrupted = data;
    corrupted[pos] = '\xFF';
    EXPECT_EQ(FirstMismatch(corrupted, false), "") << "corrupted at " << pos;
  }

  std::string truncated = data;
  truncated.pop_back();
  EXPECT_EQ(FirstMismatch(truncated, false), "");
}

}  // namespace

}  // namespace aeronet::websocket
