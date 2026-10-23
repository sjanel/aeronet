#include "aeronet/websocket-utf8.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

#include "aeronet/compiler-config.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-frame.hpp"

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>

#include "aeronet/avx2.hpp"
#define AERONET_UTF8_X86
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define AERONET_UTF8_NEON
#endif

namespace aeronet::websocket {

namespace {

/// Validate a single multibyte UTF-8 sequence starting at *ptr.
/// @param ptr  Points to 1 byte past the lead byte on entry; advanced past the full sequence on success.
/// @param end  One-past-end of the buffer.
/// @param lead The leading byte of the sequence (already consumed).
/// @return true if the sequence is valid.
constexpr bool ValidateMultibyteSequence(const uint8_t*& ptr, const uint8_t* end, uint8_t lead) {
  uint32_t remaining;
  uint32_t codepoint;
  uint32_t minCodepoint;

  if ((lead & 0xE0U) == 0xC0U) {
    remaining = 1U;
    codepoint = lead & 0x1FU;
    minCodepoint = 0x80U;
  } else if ((lead & 0xF0U) == 0xE0U) {
    remaining = 2U;
    codepoint = lead & 0x0FU;
    minCodepoint = 0x800U;
  } else if ((lead & 0xF8U) == 0xF0U) {
    remaining = 3U;
    codepoint = lead & 0x07U;
    minCodepoint = 0x10000U;
  } else {
    return false;
  }

  if (end < ptr + remaining) {
    return false;
  }

  for (decltype(remaining) idx = 0; idx < remaining; ++idx) {
    const uint8_t byte = *ptr++;
    if ((byte & 0xC0U) != 0x80U) {
      return false;
    }
    codepoint = (codepoint << 6U) | (byte & 0x3FU);
  }

  if (codepoint < minCodepoint) {
    return false;
  }
  if (codepoint >= 0xD800U && codepoint <= 0xDFFFU) {
    return false;
  }
  return codepoint <= 0x10FFFFU;
}

#if defined(AERONET_UTF8_X86) || defined(AERONET_UTF8_NEON)

// The SIMD kernels implement the lookup algorithm of John Keiser and Daniel Lemire ("Validating UTF-8 In Less Than
// One Instruction Per Byte", 2021), as in simdjson / simdutf. Each byte is classified together with the byte before
// it by three 16-entry table lookups (pshufb / tbl), indexed by the high and low nibbles of the previous byte and the
// high nibble of the current one: each table sets the bits of the error kinds compatible with its nibble, so their
// AND keeps the errors the pair of bytes actually makes. The one error a pair cannot show - a 3rd / 4th continuation
// byte missing or in excess - is found by comparing where continuation bytes must be (2 and 3 bytes after a 3 and
// 4-byte lead) with the pairs of continuation bytes found.

constexpr unsigned kTooShort = 1U << 0U;      // 11______ 0_______ or 11______ 11______: missing continuation
constexpr unsigned kTooLong = 1U << 1U;       // 0_______ 10______: continuation without lead
constexpr unsigned kOverlong3 = 1U << 2U;     // 11100000 100_____
constexpr unsigned kTooLarge = 1U << 3U;      // 11110100 1001____ / 101_____, or 11110101+ 1001____ / 101_____
constexpr unsigned kSurrogate = 1U << 4U;     // 11101101 101_____
constexpr unsigned kOverlong2 = 1U << 5U;     // 1100000_ 10______
constexpr unsigned kTooLarge1000 = 1U << 6U;  // 11110101+ 1000____
constexpr unsigned kOverlong4 = 1U << 6U;     // 11110000 1000____
constexpr unsigned kTwoConts = 1U << 7U;      // 10______ 10______: only valid as 3rd / 4th byte of a sequence
// The errors that do not depend on the low nibble of the first byte.
constexpr unsigned kCarry = kTooShort | kTooLong | kTwoConts;

// Indexed by the high nibble of the first byte.
constexpr uint8_t kByte1High[]{
    // 0_______: ASCII
    kTooLong,
    kTooLong,
    kTooLong,
    kTooLong,
    kTooLong,
    kTooLong,
    kTooLong,
    kTooLong,
    // 10______: continuation
    kTwoConts,
    kTwoConts,
    kTwoConts,
    kTwoConts,
    // 1100____: 2-byte lead
    kTooShort | kOverlong2,
    // 1101____: 2-byte lead
    kTooShort,
    // 1110____: 3-byte lead
    kTooShort | kOverlong3 | kSurrogate,
    // 1111____: 4-byte lead (or invalid byte)
    kTooShort | kTooLarge | kTooLarge1000 | kOverlong4,
};

// Indexed by the low nibble of the first byte.
constexpr uint8_t kByte1Low[]{
    // ____0000
    kCarry | kOverlong3 | kOverlong2 | kOverlong4,
    // ____0001
    kCarry | kOverlong2,
    // ____001_
    kCarry,
    kCarry,
    // ____0100
    kCarry | kTooLarge,
    // ____0101
    kCarry | kTooLarge | kTooLarge1000,
    // ____011_
    kCarry | kTooLarge | kTooLarge1000,
    kCarry | kTooLarge | kTooLarge1000,
    // ____1___
    kCarry | kTooLarge | kTooLarge1000,
    kCarry | kTooLarge | kTooLarge1000,
    kCarry | kTooLarge | kTooLarge1000,
    kCarry | kTooLarge | kTooLarge1000,
    kCarry | kTooLarge | kTooLarge1000,
    // ____1101
    kCarry | kTooLarge | kTooLarge1000 | kSurrogate,
    // ____111_
    kCarry | kTooLarge | kTooLarge1000,
    kCarry | kTooLarge | kTooLarge1000,
};

// Indexed by the high nibble of the second byte.
constexpr uint8_t kByte2High[]{
    // 0_______: ASCII
    kTooShort,
    kTooShort,
    kTooShort,
    kTooShort,
    kTooShort,
    kTooShort,
    kTooShort,
    kTooShort,
    // 1000____
    kTooLong | kOverlong2 | kTwoConts | kOverlong3 | kTooLarge1000 | kOverlong4,
    // 1001____
    kTooLong | kOverlong2 | kTwoConts | kOverlong3 | kTooLarge,
    // 101_____
    kTooLong | kOverlong2 | kTwoConts | kSurrogate | kTooLarge,
    kTooLong | kOverlong2 | kTwoConts | kSurrogate | kTooLarge,
    // 11______: lead
    kTooShort,
    kTooShort,
    kTooShort,
    kTooShort,
};

// A block ends in the middle of a sequence when one of its last 3 bytes is above the matching value: 0xF0 - 1, 0xE0 - 1
// and 0xC0 - 1 (lead of a 4, 3 and 2-byte sequence respectively). The 16-byte kernels use the last 16 bytes.
constexpr uint8_t kIncompleteMax[]{
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0xDF, 0xBF,
};

// Subtracted with saturation from the bytes 2 and 3 positions back, they leave the high bit set only for a 3 or 4-byte
// lead (111_____ and 1111____): the positions where a continuation byte must follow another one.
constexpr uint8_t kThirdByteSub = 0xE0U - 0x80U;
constexpr uint8_t kFourthByteSub = 0xF0U - 0x80U;

// The bytes read by a kernel: mutable when it unmasks them in place (kUnmask).
template <bool kUnmask>
using KernelBytes = std::conditional_t<kUnmask, uint8_t, const uint8_t>;

// Loads the word at ptr with byte i in bits [8i, 8i + 8).
template <class T>
T LoadLittleEndian(const uint8_t* ptr) {
  T value;
  std::memcpy(&value, ptr, sizeof(T));
  if constexpr (std::endian::native == std::endian::big) {
    value = std::byteswap(value);
  }
  return value;
}

// Stores the word 'value' at ptr, its bits [8i, 8i + 8) at ptr + i.
template <class T>
void StoreLittleEndian(uint8_t* ptr, T value) {
  if constexpr (std::endian::native == std::endian::big) {
    value = std::byteswap(value);
  }
  std::memcpy(ptr, &value, sizeof(T));
}

// The masking key over 8 bytes, as a little-endian word (see LoadLittleEndian): the mask of the 8 bytes starting at a
// multiple of 4 from the start of the payload.
uint64_t LittleEndianMask(MaskingKey maskingKey) {
  const auto key = LoadLittleEndian<uint32_t>(reinterpret_cast<const uint8_t*>(&maskingKey));
  return key | (static_cast<uint64_t>(key) << 32U);
}

// The mask (from LittleEndianMask) of the bytes starting 'offset' bytes after a multiple of 4 from the start of the
// payload: its low 32 bits mask the 4 bytes starting there.
constexpr uint64_t ShiftMask(uint64_t mask, std::size_t offset) noexcept {
  return std::rotr(mask, static_cast<int>((offset % kMaskingKeySize) * 8U));
}

// Loads the word 'offset' bytes after ptr as LoadLittleEndian, unmasked with 'mask' (from LittleEndianMask, ptr being
// at a multiple of 4 from the start of the payload) if kUnmask.
template <class T, bool kUnmask>
T LoadWord(const uint8_t* ptr, std::size_t offset, uint64_t mask) {
  T value = LoadLittleEndian<T>(ptr + offset);
  if constexpr (kUnmask) {
    value ^= static_cast<T>(ShiftMask(mask, offset));
  }
  return value;
}

// The 16-byte block made of the 'size' (< 16) bytes at ptr followed by zeros, as two little-endian words.
struct PartialBlock {
  uint64_t low{};
  uint64_t high{};
};

// Stores at ptr the 'size' (< 16) first bytes of 'block', with the overlapping stores matching the loads of
// LoadPartialBlock (bytes stored twice get the same value).
AERONET_ALWAYS_INLINE void StorePartialBlock(uint8_t* ptr, std::size_t size, PartialBlock block) {
  if (size >= 8U) {
    StoreLittleEndian(ptr, block.low);
    if (size != 8U) {
      // The last 8 bytes: the end of 'low', then the start of 'high'.
      StoreLittleEndian(ptr + size - 8U, (block.low >> ((size - 8U) * 8U)) | (block.high << ((16U - size) * 8U)));
    }
  } else if (size >= 4U) {
    StoreLittleEndian(ptr, static_cast<uint32_t>(block.low));
    StoreLittleEndian(ptr + size - 4U, static_cast<uint32_t>(block.low >> ((size - 4U) * 8U)));
  } else if (size != 0U) {
    ptr[0] = static_cast<uint8_t>(block.low);
    ptr[size / 2] = static_cast<uint8_t>(block.low >> (size / 2U * 8U));
    ptr[size - 1U] = static_cast<uint8_t>(block.low >> ((size - 1U) * 8U));
  }
}

// Reads the last partial block of a small input with overlapping word loads that stay within [ptr, ptr + size). The
// ASCII zeros padding it also reveal a sequence truncated at the end. Unlike a copy to a zeroed buffer, the vector
// built from these registers does not stall on store forwarding.
// If kUnmask, each word is unmasked once loaded (ptr being at a multiple of 4 from the start of the payload), then the
// block is stored back: the bytes are unmasked in place without being read again from memory.
// Inlined into the kernels, so that the block stays in registers.
template <bool kUnmask>
AERONET_ALWAYS_INLINE PartialBlock LoadPartialBlock(KernelBytes<kUnmask>* ptr, std::size_t size, uint64_t mask) {
  PartialBlock block;
  if (size >= 8U) {
    block.low = LoadWord<uint64_t, kUnmask>(ptr, 0, mask);
    if (size != 8U) {
      // The last 8 bytes, without those already in 'low'.
      block.high = LoadWord<uint64_t, kUnmask>(ptr, size - 8U, mask) >> ((16U - size) * 8U);
    }
  } else if (size >= 4U) {
    block.low = LoadWord<uint32_t, kUnmask>(ptr, 0, mask) |
                (static_cast<uint64_t>(LoadWord<uint32_t, kUnmask>(ptr, size - 4U, mask)) << ((size - 4U) * 8U));
  } else if (size != 0U) {
    // First, middle and last bytes (the same byte twice for a size of 1 or 2).
    block.low = LoadWord<uint8_t, kUnmask>(ptr, 0, mask) |
                (static_cast<uint64_t>(LoadWord<uint8_t, kUnmask>(ptr, size / 2, mask)) << (size / 2U * 8U)) |
                (static_cast<uint64_t>(LoadWord<uint8_t, kUnmask>(ptr, size - 1U, mask)) << ((size - 1U) * 8U));
  }
  if constexpr (kUnmask) {
    StorePartialBlock(ptr, size, block);
  }
  return block;
}

#endif

#ifdef AERONET_UTF8_X86

template <bool kUnmask>
AERONET_ALWAYS_INLINE __m128i LoadPartialBlock128(KernelBytes<kUnmask>* ptr, std::size_t size, uint64_t mask) {
  const PartialBlock block = LoadPartialBlock<kUnmask>(ptr, size, mask);
#if defined(__x86_64__) || defined(_M_X64)
  // From the registers: GCC builds _mm_set_epi64x through the stack (a 16-byte load stalling on two 8-byte stores).
  return _mm_unpacklo_epi64(_mm_cvtsi64_si128(static_cast<long long>(block.low)),
                            _mm_cvtsi64_si128(static_cast<long long>(block.high)));
#else
  return _mm_set_epi64x(static_cast<long long>(block.high), static_cast<long long>(block.low));
#endif
}

// The constant vectors of the SSSE3 kernel.
struct Ssse3Constants {
  __m128i byte1High;
  __m128i byte1Low;
  __m128i byte2High;
  __m128i incompleteMax;
};

// Accumulates in 'error' the errors of the 16-byte block 'input' following 'prevInput' (of which only the last 3 bytes
// matter), and returns the bytes of 'input' that make it end in the middle of a sequence (zero if none).
AERONET_ALWAYS_INLINE
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("ssse3")))
#endif
__m128i CheckBlockSsse3(const Ssse3Constants& constants, __m128i input, __m128i prevInput, __m128i& error) {
  const __m128i lowNibbleMask = _mm_set1_epi8(0x0F);
  const __m128i prev1 = _mm_alignr_epi8(input, prevInput, 15);
  const __m128i byte1High =
      _mm_shuffle_epi8(constants.byte1High, _mm_and_si128(_mm_srli_epi16(prev1, 4), lowNibbleMask));
  const __m128i byte1Low = _mm_shuffle_epi8(constants.byte1Low, _mm_and_si128(prev1, lowNibbleMask));
  const __m128i byte2High =
      _mm_shuffle_epi8(constants.byte2High, _mm_and_si128(_mm_srli_epi16(input, 4), lowNibbleMask));
  const __m128i specialCases = _mm_and_si128(_mm_and_si128(byte1High, byte1Low), byte2High);

  const __m128i prev2 = _mm_alignr_epi8(input, prevInput, 14);
  const __m128i prev3 = _mm_alignr_epi8(input, prevInput, 13);
  const __m128i mustBe23Continuation =
      _mm_or_si128(_mm_subs_epu8(prev2, _mm_set1_epi8(static_cast<char>(kThirdByteSub))),
                   _mm_subs_epu8(prev3, _mm_set1_epi8(static_cast<char>(kFourthByteSub))));
  const __m128i mustBe23Continuation80 =
      _mm_and_si128(mustBe23Continuation, _mm_set1_epi8(static_cast<char>(kTwoConts)));
  error = _mm_or_si128(error, _mm_xor_si128(mustBe23Continuation80, specialCases));

  return _mm_subs_epu8(input, constants.incompleteMax);
}

// The constant vectors of the AVX2 kernel.
struct Avx2Constants {
  __m256i byte1High;
  __m256i byte1Low;
  __m256i byte2High;
  __m256i incompleteMax;
};

// Accumulates in 'error' the errors of the 32-byte block 'input' following 'prevInput' (of which only the last 3 bytes
// matter), and returns the bytes of 'input' that make it end in the middle of a sequence (zero if none).
AERONET_ALWAYS_INLINE
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
__m256i CheckBlockAvx2(const Avx2Constants& constants, __m256i input, __m256i prevInput, __m256i& error) {
  const __m256i lowNibbleMask = _mm256_set1_epi8(0x0F);
  // alignr shifts within each 128-bit lane: feed it the bytes preceding each lane (high lane of prevInput, low lane of
  // input).
  const __m256i prevLanes = _mm256_permute2x128_si256(prevInput, input, 0x21);
  const __m256i prev1 = _mm256_alignr_epi8(input, prevLanes, 15);
  const __m256i byte1High =
      _mm256_shuffle_epi8(constants.byte1High, _mm256_and_si256(_mm256_srli_epi16(prev1, 4), lowNibbleMask));
  const __m256i byte1Low = _mm256_shuffle_epi8(constants.byte1Low, _mm256_and_si256(prev1, lowNibbleMask));
  const __m256i byte2High =
      _mm256_shuffle_epi8(constants.byte2High, _mm256_and_si256(_mm256_srli_epi16(input, 4), lowNibbleMask));
  const __m256i specialCases = _mm256_and_si256(_mm256_and_si256(byte1High, byte1Low), byte2High);

  const __m256i prev2 = _mm256_alignr_epi8(input, prevLanes, 14);
  const __m256i prev3 = _mm256_alignr_epi8(input, prevLanes, 13);
  const __m256i mustBe23Continuation =
      _mm256_or_si256(_mm256_subs_epu8(prev2, _mm256_set1_epi8(static_cast<char>(kThirdByteSub))),
                      _mm256_subs_epu8(prev3, _mm256_set1_epi8(static_cast<char>(kFourthByteSub))));
  const __m256i mustBe23Continuation80 =
      _mm256_and_si256(mustBe23Continuation, _mm256_set1_epi8(static_cast<char>(kTwoConts)));
  error = _mm256_or_si256(error, _mm256_xor_si256(mustBe23Continuation80, specialCases));

  return _mm256_subs_epu8(input, constants.incompleteMax);
}

#elifdef AERONET_UTF8_NEON

// The constant vectors of the NEON kernel.
struct NeonConstants {
  uint8x16_t byte1High;
  uint8x16_t byte1Low;
  uint8x16_t byte2High;
  uint8x16_t incompleteMax;
};

// Accumulates in 'error' the errors of the 16-byte block 'input' following 'prevInput' (of which only the last 3 bytes
// matter), and returns the bytes of 'input' that make it end in the middle of a sequence (zero if none).
AERONET_ALWAYS_INLINE uint8x16_t CheckBlockNeon(const NeonConstants& constants, uint8x16_t input, uint8x16_t prevInput,
                                                uint8x16_t& error) {
  const uint8x16_t prev1 = vextq_u8(prevInput, input, 15);
  const uint8x16_t byte1High = vqtbl1q_u8(constants.byte1High, vshrq_n_u8(prev1, 4));
  const uint8x16_t byte1Low = vqtbl1q_u8(constants.byte1Low, vandq_u8(prev1, vdupq_n_u8(0x0F)));
  const uint8x16_t byte2High = vqtbl1q_u8(constants.byte2High, vshrq_n_u8(input, 4));
  const uint8x16_t specialCases = vandq_u8(vandq_u8(byte1High, byte1Low), byte2High);

  const uint8x16_t prev2 = vextq_u8(prevInput, input, 14);
  const uint8x16_t prev3 = vextq_u8(prevInput, input, 13);
  const uint8x16_t mustBe23Continuation =
      vorrq_u8(vqsubq_u8(prev2, vdupq_n_u8(kThirdByteSub)), vqsubq_u8(prev3, vdupq_n_u8(kFourthByteSub)));
  const uint8x16_t mustBe23Continuation80 = vandq_u8(mustBe23Continuation, vdupq_n_u8(static_cast<uint8_t>(kTwoConts)));
  error = vorrq_u8(error, veorq_u8(mustBe23Continuation80, specialCases));

  return vqsubq_u8(input, constants.incompleteMax);
}

#endif

// The SIMD kernels check the full blocks, then the last partial block if any. For an input of at least one block and 4
// bytes, the last partial block is replaced by the full block ending at the end of the input: its bytes overlapping the
// previous block are checked twice, harmlessly. Of the previous block, the check only uses the last 3 bytes, those
// preceding the first bytes of the block: the kernels set them to the 3 bytes before the re-read block with a broadcast
// of the 4-byte word preceding it (vector bytes 4i + 1, 4i + 2 and 4i + 3). The last partial block of a smaller input
// is padded with zeros instead (see LoadPartialBlock).
// An ASCII block is skipped, without becoming the previous block of the next one: the last bytes of a block ending with
// a complete sequence check the next bytes the same way as ASCII bytes. A block ending in the middle of a sequence
// followed by an ASCII block is an error by itself (prevIncomplete).
// With kUnmask, the kernels unmask the input in the same pass: each block is unmasked once loaded, stored back, then
// checked. The re-read last block and the word preceding it are loaded before the loop, which unmasks their first bytes
// in place: they are unmasked with the mask of their offset in the payload, and the block is stored back whole (its
// bytes already unmasked by the loop with the same values).

#ifdef AERONET_UTF8_X86

template <bool kUnmask>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("ssse3")))
#endif
bool ValidateSsse3(KernelBytes<kUnmask>* ptr, std::size_t size, [[maybe_unused]] uint64_t mask) noexcept {
  static constexpr std::size_t kBlockSize = sizeof(__m128i);

  const Ssse3Constants constants{
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(kByte1High)),
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(kByte1Low)),
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(kByte2High)),
      _mm_loadu_si128(reinterpret_cast<const __m128i*>(kIncompleteMax + 16)),
  };

  // The blocks of the loop are all at a multiple of 16 from the start of the payload.
  [[maybe_unused]] const __m128i blockMask = _mm_set1_epi64x(static_cast<long long>(mask));
  const bool rereadsLastBlock = size >= kBlockSize + sizeof(uint32_t);
  [[maybe_unused]] __m128i lastBlock = _mm_setzero_si128();
  [[maybe_unused]] uint32_t lastPrev = 0;
  if constexpr (kUnmask) {
    if (rereadsLastBlock) {
      lastBlock = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr + size - kBlockSize));
      lastPrev = LoadLittleEndian<uint32_t>(ptr + size - kBlockSize - sizeof(uint32_t));
    }
  }

  std::size_t remaining = size;
  __m128i error = _mm_setzero_si128();
  __m128i prevInput = _mm_setzero_si128();
  __m128i prevIncomplete = _mm_setzero_si128();
  for (; remaining >= kBlockSize; ptr += kBlockSize, remaining -= kBlockSize) {
    __m128i input = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));
    if constexpr (kUnmask) {
      input = _mm_xor_si128(input, blockMask);
      _mm_storeu_si128(reinterpret_cast<__m128i*>(ptr), input);
    }
    if (_mm_movemask_epi8(input) == 0) {
      error = _mm_or_si128(error, prevIncomplete);
    } else {
      prevIncomplete = CheckBlockSsse3(constants, input, prevInput, error);
      prevInput = input;
    }
  }
  if (remaining != 0) {
    __m128i input;
    if (rereadsLastBlock) {
      auto* last = ptr + remaining - kBlockSize;
      if constexpr (kUnmask) {
        const auto lastMask = static_cast<uint32_t>(ShiftMask(mask, size - kBlockSize));
        input = _mm_xor_si128(lastBlock, _mm_set1_epi32(static_cast<int>(lastMask)));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(last), input);
        prevInput = _mm_set1_epi32(static_cast<int>(lastPrev ^ lastMask));
      } else {
        prevInput = _mm_set1_epi32(static_cast<int>(LoadLittleEndian<uint32_t>(last - sizeof(uint32_t))));
        input = _mm_loadu_si128(reinterpret_cast<const __m128i*>(last));
      }
    } else {
      input = LoadPartialBlock128<kUnmask>(ptr, remaining, mask);
    }
    if (_mm_movemask_epi8(input) != 0) {
      prevIncomplete = CheckBlockSsse3(constants, input, prevInput, error);
    }
  }
  error = _mm_or_si128(error, prevIncomplete);
  return _mm_movemask_epi8(_mm_cmpeq_epi8(error, _mm_setzero_si128())) == 0xFFFFU;
}

template <bool kUnmask>
#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
bool ValidateAvx2(KernelBytes<kUnmask>* ptr, std::size_t size, [[maybe_unused]] uint64_t mask) noexcept {
  static constexpr std::size_t kBlockSize = sizeof(__m256i);
  static constexpr std::size_t kHalfBlockSize = sizeof(__m128i);

  const Avx2Constants constants{
      _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(kByte1High))),
      _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(kByte1Low))),
      _mm256_broadcastsi128_si256(_mm_loadu_si128(reinterpret_cast<const __m128i*>(kByte2High))),
      _mm256_loadu_si256(reinterpret_cast<const __m256i*>(kIncompleteMax)),
  };

  // The blocks of the loop are all at a multiple of 32 from the start of the payload.
  [[maybe_unused]] const __m256i blockMask = _mm256_set1_epi64x(static_cast<long long>(mask));
  const bool rereadsLastBlock = size >= kBlockSize + sizeof(uint32_t);
  [[maybe_unused]] __m256i lastBlock = _mm256_setzero_si256();
  [[maybe_unused]] uint32_t lastPrev = 0;
  if constexpr (kUnmask) {
    if (rereadsLastBlock) {
      lastBlock = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr + size - kBlockSize));
      lastPrev = LoadLittleEndian<uint32_t>(ptr + size - kBlockSize - sizeof(uint32_t));
    }
  }

  std::size_t remaining = size;
  __m256i error = _mm256_setzero_si256();
  __m256i prevInput = _mm256_setzero_si256();
  __m256i prevIncomplete = _mm256_setzero_si256();
  for (; remaining >= kBlockSize; ptr += kBlockSize, remaining -= kBlockSize) {
    __m256i input = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(ptr));
    if constexpr (kUnmask) {
      input = _mm256_xor_si256(input, blockMask);
      _mm256_storeu_si256(reinterpret_cast<__m256i*>(ptr), input);
    }
    if (_mm256_movemask_epi8(input) == 0) {
      error = _mm256_or_si256(error, prevIncomplete);
    } else {
      prevIncomplete = CheckBlockAvx2(constants, input, prevInput, error);
      prevInput = input;
    }
  }
  if (remaining != 0) {
    __m256i input;
    if (rereadsLastBlock) {
      auto* last = ptr + remaining - kBlockSize;
      if constexpr (kUnmask) {
        const auto lastMask = static_cast<uint32_t>(ShiftMask(mask, size - kBlockSize));
        input = _mm256_xor_si256(lastBlock, _mm256_set1_epi32(static_cast<int>(lastMask)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(last), input);
        prevInput = _mm256_set1_epi32(static_cast<int>(lastPrev ^ lastMask));
      } else {
        prevInput = _mm256_set1_epi32(static_cast<int>(LoadLittleEndian<uint32_t>(last - sizeof(uint32_t))));
        input = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(last));
      }
    } else if (remaining >= kHalfBlockSize) {
      __m128i low = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr));
      if constexpr (kUnmask) {
        low = _mm_xor_si128(low, _mm256_castsi256_si128(blockMask));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(ptr), low);
      }
      input =
          _mm256_set_m128i(LoadPartialBlock128<kUnmask>(ptr + kHalfBlockSize, remaining - kHalfBlockSize, mask), low);
    } else {
      input = _mm256_set_m128i(_mm_setzero_si128(), LoadPartialBlock128<kUnmask>(ptr, remaining, mask));
    }
    if (_mm256_movemask_epi8(input) != 0) {
      prevIncomplete = CheckBlockAvx2(constants, input, prevInput, error);
    }
  }
  error = _mm256_or_si256(error, prevIncomplete);
  return _mm256_testz_si256(error, error) != 0;
}

#elifdef AERONET_UTF8_NEON

template <bool kUnmask>
bool ValidateNeon(KernelBytes<kUnmask>* ptr, std::size_t size, [[maybe_unused]] uint64_t mask) noexcept {
  static constexpr std::size_t kBlockSize = sizeof(uint8x16_t);

  const NeonConstants constants{
      vld1q_u8(kByte1High),
      vld1q_u8(kByte1Low),
      vld1q_u8(kByte2High),
      vld1q_u8(kIncompleteMax + 16),
  };

  // The blocks of the loop are all at a multiple of 16 from the start of the payload.
  [[maybe_unused]] const uint8x16_t blockMask = vreinterpretq_u8_u64(vdupq_n_u64(mask));
  const bool rereadsLastBlock = size >= kBlockSize + sizeof(uint32_t);
  [[maybe_unused]] uint8x16_t lastBlock = vdupq_n_u8(0);
  [[maybe_unused]] uint32_t lastPrev = 0;
  if constexpr (kUnmask) {
    if (rereadsLastBlock) {
      lastBlock = vld1q_u8(ptr + size - kBlockSize);
      lastPrev = LoadLittleEndian<uint32_t>(ptr + size - kBlockSize - sizeof(uint32_t));
    }
  }

  std::size_t remaining = size;
  uint8x16_t error = vdupq_n_u8(0);
  uint8x16_t prevInput = vdupq_n_u8(0);
  uint8x16_t prevIncomplete = vdupq_n_u8(0);
  for (; remaining >= kBlockSize; ptr += kBlockSize, remaining -= kBlockSize) {
    uint8x16_t input = vld1q_u8(ptr);
    if constexpr (kUnmask) {
      input = veorq_u8(input, blockMask);
      vst1q_u8(ptr, input);
    }
    if (vmaxvq_u8(input) < 0x80U) {
      error = vorrq_u8(error, prevIncomplete);
    } else {
      prevIncomplete = CheckBlockNeon(constants, input, prevInput, error);
      prevInput = input;
    }
  }
  if (remaining != 0) {
    uint8x16_t input;
    if (rereadsLastBlock) {
      auto* last = ptr + remaining - kBlockSize;
      if constexpr (kUnmask) {
        const auto lastMask = static_cast<uint32_t>(ShiftMask(mask, size - kBlockSize));
        input = veorq_u8(lastBlock, vreinterpretq_u8_u32(vdupq_n_u32(lastMask)));
        vst1q_u8(last, input);
        prevInput = vreinterpretq_u8_u32(vdupq_n_u32(lastPrev ^ lastMask));
      } else {
        prevInput = vreinterpretq_u8_u32(vdupq_n_u32(LoadLittleEndian<uint32_t>(last - sizeof(uint32_t))));
        input = vld1q_u8(last);
      }
    } else {
      const PartialBlock block = LoadPartialBlock<kUnmask>(ptr, remaining, mask);
      input = vcombine_u8(vcreate_u8(block.low), vcreate_u8(block.high));
    }
    if (vmaxvq_u8(input) >= 0x80U) {
      prevIncomplete = CheckBlockNeon(constants, input, prevInput, error);
    }
  }
  error = vorrq_u8(error, prevIncomplete);
  return vmaxvq_u8(error) == 0;
}

#endif

}  // namespace

namespace detail {

bool IsValidUtf8Scalar(std::span<const std::byte> data) noexcept {
  static constexpr uint64_t kHighBits = 0x8080808080808080ULL;

  const auto* ptr = reinterpret_cast<const uint8_t*>(data.data());
  const auto* end = ptr + data.size();
  while (ptr != end) {
    if (end - ptr >= static_cast<std::ptrdiff_t>(sizeof(uint64_t))) {
      uint64_t word;
      std::memcpy(&word, ptr, sizeof(word));
      if ((word & kHighBits) == 0) {
        ptr += sizeof(word);
        continue;
      }
    }
    const uint8_t byte = *ptr++;
    if (byte >= 0x80 && !ValidateMultibyteSequence(ptr, end, byte)) {
      return false;
    }
  }
  return true;
}

#ifdef AERONET_UTF8_X86

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("ssse3")))
#endif
bool IsValidUtf8Ssse3(std::span<const std::byte> data) noexcept {
  return ValidateSsse3<false>(reinterpret_cast<const uint8_t*>(data.data()), data.size(), 0);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
bool IsValidUtf8Avx2(std::span<const std::byte> data) noexcept {
  return ValidateAvx2<false>(reinterpret_cast<const uint8_t*>(data.data()), data.size(), 0);
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("ssse3")))
#endif
bool UnmaskAndValidateUtf8Ssse3(std::span<std::byte> data, MaskingKey maskingKey) noexcept {
  return ValidateSsse3<true>(reinterpret_cast<uint8_t*>(data.data()), data.size(), LittleEndianMask(maskingKey));
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((target("avx2")))
#endif
bool UnmaskAndValidateUtf8Avx2(std::span<std::byte> data, MaskingKey maskingKey) noexcept {
  return ValidateAvx2<true>(reinterpret_cast<uint8_t*>(data.data()), data.size(), LittleEndianMask(maskingKey));
}

#elifdef AERONET_UTF8_NEON

bool IsValidUtf8Neon(std::span<const std::byte> data) noexcept {
  return ValidateNeon<false>(reinterpret_cast<const uint8_t*>(data.data()), data.size(), 0);
}

bool UnmaskAndValidateUtf8Neon(std::span<std::byte> data, MaskingKey maskingKey) noexcept {
  return ValidateNeon<true>(reinterpret_cast<uint8_t*>(data.data()), data.size(), LittleEndianMask(maskingKey));
}

#endif

}  // namespace detail

bool IsValidUtf8(std::span<const std::byte> data) noexcept {
#ifdef AERONET_UTF8_NEON
  return detail::IsValidUtf8Neon(data);
#elifdef AERONET_UTF8_X86
  if (HasAvx2()) {
    return detail::IsValidUtf8Avx2(data);
  }
  if (HasSsse3()) {
    return detail::IsValidUtf8Ssse3(data);
  }
#endif
  return detail::IsValidUtf8Scalar(data);
}

bool UnmaskAndValidateUtf8(std::span<std::byte> data, MaskingKey maskingKey) noexcept {
#ifdef AERONET_UTF8_NEON
  return detail::UnmaskAndValidateUtf8Neon(data, maskingKey);
#elifdef AERONET_UTF8_X86
  if (HasAvx2()) {
    return detail::UnmaskAndValidateUtf8Avx2(data, maskingKey);
  }
  if (HasSsse3()) {
    return detail::UnmaskAndValidateUtf8Ssse3(data, maskingKey);
  }
#endif
  // Two passes without SIMD kernel: the scalar validation is much slower than the unmasking anyway.
  ApplyMask(data, maskingKey);
  return detail::IsValidUtf8Scalar(data);
}

}  // namespace aeronet::websocket
