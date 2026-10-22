// Microbenchmark for the UTF-8 validation of WebSocket text messages.
//
// Measures `aeronet::websocket::IsValidUtf8()` and each of its kernels (scalar, SSSE3 / AVX2 or NEON lookup
// algorithm) on valid text of various scripts and sizes, against a frozen copy of the previous implementation (SIMD
// skip of ASCII blocks, scalar decoding of each multibyte character).
// Also measures the unmasking of a masked text message (as received from a client) in the same pass as its validation
// (`UnmaskAndValidateUtf8()`), against `ApplyMask()` followed by `IsValidUtf8()`.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aeronet/avx2.hpp"
#include "aeronet/compiler-config.hpp"
#include "aeronet/websocket-constants.hpp"
#include "aeronet/websocket-frame.hpp"
#include "aeronet/websocket-utf8.hpp"

#ifdef __SSE2__
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

namespace {

using namespace aeronet::websocket;

// Frozen copy of the previous implementation, for a direct baseline.
constexpr bool PreviousValidateMultibyteSequence(const uint8_t*& ptr, const uint8_t* end, uint8_t lead) {
  uint32_t remaining;
  uint32_t codepoint;
  uint32_t minCodepoint;
  if ((lead & 0xE0U) == 0xC0) {
    remaining = 1;
    codepoint = lead & 0x1FU;
    minCodepoint = 0x80;
  } else if ((lead & 0xF0U) == 0xE0) {
    remaining = 2;
    codepoint = lead & 0x0FU;
    minCodepoint = 0x800;
  } else if ((lead & 0xF8U) == 0xF0) {
    remaining = 3;
    codepoint = lead & 0x07U;
    minCodepoint = 0x10000;
  } else {
    return false;
  }
  if (end < ptr + remaining) {
    return false;
  }
  for (decltype(remaining) idx = 0; idx < remaining; ++idx) {
    const uint8_t byte = *ptr++;
    if ((byte & 0xC0U) != 0x80) {
      return false;
    }
    codepoint = (codepoint << 6U) | (byte & 0x3FU);
  }
  if (codepoint < minCodepoint) {
    return false;
  }
  if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
    return false;
  }
  return codepoint <= 0x10FFFF;
}

// Not inlined, as the library kernels it is compared with (calls into another translation unit).
AERONET_NOINLINE bool PreviousValidateUtf8(std::span<const std::byte> data) noexcept {
  const auto* ptr = reinterpret_cast<const uint8_t*>(data.data());
  const auto* end = ptr + data.size();
#ifdef __SSE2__
  while (ptr + 16 <= end) {
    const int mask = _mm_movemask_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr)));
    if (mask == 0) {
      ptr += 16;
      continue;
    }
    ptr += static_cast<std::size_t>(__builtin_ctz(static_cast<unsigned>(mask)));
    const uint8_t lead = *ptr++;
    if (!PreviousValidateMultibyteSequence(ptr, end, lead)) {
      return false;
    }
  }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
  while (ptr + 16 <= end) {
    if (vmaxvq_u8(vld1q_u8(ptr)) < 0x80) {
      ptr += 16;
      continue;
    }
    while (ptr < end && *ptr <= 0x7F) {
      ++ptr;
    }
    if (ptr >= end) {
      break;
    }
    const uint8_t lead = *ptr++;
    if (!PreviousValidateMultibyteSequence(ptr, end, lead)) {
      return false;
    }
  }
#endif
  while (ptr < end) {
    const uint8_t byte = *ptr++;
    if (byte > 0x7F && !PreviousValidateMultibyteSequence(ptr, end, byte)) {
      return false;
    }
  }
  return true;
}

// Text samples, repeated to the benchmarked size.
constexpr std::array<std::string_view, 5> kSamples = {
    // JSON-like ASCII
    R"({"type":"chat","room":"general","user":"alice","text":"see you at the meeting tomorrow","ts":1714380000},)",
    // French: mostly ASCII, an accented letter every ~12 bytes
    "Le c\xC5\x93ur d\xC3\xA9\xC3\xA7u mais l'\xC3\xA2me plut\xC3\xB4t na\xC3\xAFve, Lou\xC3\xBFs r\xC3\xAAva de "
    "crapa\xC3\xBCter en cano\xC3\xAB au del\xC3\xA0 des \xC3\xAEles, pr\xC3\xA8s du m\xC3\xA4lstr\xC3\xB6m. ",
    // Chinese: 3-byte characters
    "\xE6\x88\x91\xE8\x83\xBD\xE5\x90\x9E\xE4\xB8\x8B\xE7\x8E\xBB\xE7\x92\x83\xE8\x80\x8C\xE4\xB8\x8D\xE4\xBC\xA4"
    "\xE8\xBA\xAB\xE4\xBD\x93\xE3\x80\x82",
    // Emojis: 4-byte characters
    "\xF0\x9F\x98\x80\xF0\x9F\x8E\x89\xF0\x9F\x8C\x8D\xF0\x9F\x9A\x80\xF0\x9F\x91\x8D\xF0\x9F\x94\xA5",
    // JSON mixing ASCII, Japanese and emojis
    R"({"user":")"
    "\xE5\xB1\xB1\xE7\x94\xB0\xE5\xA4\xAA\xE9\x83\x8E"
    R"(","msg":")"
    "\xE3\x81\x93\xE3\x82\x93\xE3\x81\xAB\xE3\x81\xA1\xE3\x81\xAF \xF0\x9F\x8C\xB8"
    R"( hello","id":12345},)",
};
constexpr std::array<std::string_view, kSamples.size()> kSampleNames = {"ascii", "latin", "cjk", "emoji", "mixed"};

// Valid UTF-8 of exactly 'size' bytes: whole characters of the sample, padded with spaces.
std::string MakeText(std::string_view sample, std::size_t size) {
  std::string text;
  text.reserve(size);
  for (std::size_t pos = 0; text.size() < size; pos = (pos + 1) % sample.size()) {
    std::size_t charSize = 1;
    while (pos + charSize < sample.size() && (static_cast<uint8_t>(sample[pos + charSize]) & 0xC0U) == 0x80U) {
      ++charSize;
    }
    if (text.size() + charSize > size) {
      text.resize(size, ' ');
      break;
    }
    text.append(sample.substr(pos, charSize));
    pos += charSize - 1;
  }
  return text;
}

enum class Isa : std::uint8_t { None, Ssse3, Avx2 };

template <bool (*Validate)(std::span<const std::byte>) noexcept, Isa kIsa = Isa::None>
void BM_Utf8(benchmark::State& state) {
  if ((kIsa == Isa::Ssse3 && !aeronet::HasSsse3()) || (kIsa == Isa::Avx2 && !aeronet::HasAvx2())) {
    state.SkipWithMessage("instruction set not supported by this CPU");
    return;
  }
  const auto sampleIdx = static_cast<std::size_t>(state.range(0));
  const auto size = static_cast<std::size_t>(state.range(1));
  const std::string text = MakeText(kSamples[sampleIdx], size);
  const std::span<const std::byte> bytes = std::as_bytes(std::span(text));
  if (!Validate(bytes)) {
    state.SkipWithError("benchmark text is not valid UTF-8");
    return;
  }

  for ([[maybe_unused]] auto iter : state) {
    benchmark::DoNotOptimize(bytes.data());
    benchmark::DoNotOptimize(Validate(bytes));
  }

  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
  state.SetLabel(std::string(kSampleNames[sampleIdx]));
}

void Args(benchmark::Benchmark* bench) {
  bench->ArgNames({"text", "size"})->ArgsProduct({{0, 1, 2, 3, 4}, {16, 40, 100, 256, 4096, 65536}});
}

enum class Unmasking : std::uint8_t {
  RestoreOnly,  // only restores the masked payload, the cost included in the others
  TwoPasses,    // ApplyMask(), then IsValidUtf8()
  SinglePass,   // UnmaskAndValidateUtf8()
};

// Unmasks and validates masked text, restoring it after each iteration. The iterations cycle over copies of the masked
// text spanning 256 KiB, so that a copy was restored long before being used again (as received bytes): the restoring
// stores do not interfere with the measured loads.
template <Unmasking kUnmasking>
void BM_Unmask(benchmark::State& state) {
  static constexpr MaskingKey kMaskingKey = 0x8E17A5C3U;
  static constexpr std::size_t kCopiesBytes = 256UL * 1024UL;

  const auto sampleIdx = static_cast<std::size_t>(state.range(0));
  const auto size = static_cast<std::size_t>(state.range(1));
  const std::string text = MakeText(kSamples[sampleIdx], size);
  std::vector<std::byte> masked(size);
  aeronet::websocket::AppendMasked(std::as_bytes(std::span(text)), masked.data(), kMaskingKey);
  const std::size_t nbCopies = std::max<std::size_t>(1, kCopiesBytes / size);
  std::vector<std::byte> copies(nbCopies * size);
  for (std::size_t copyIdx = 0; copyIdx < nbCopies; ++copyIdx) {
    std::ranges::copy(masked, copies.begin() + static_cast<std::ptrdiff_t>(copyIdx * size));
  }
  if (!UnmaskAndValidateUtf8(std::span(masked), kMaskingKey) ||
      !std::ranges::equal(masked, std::as_bytes(std::span(text)))) {
    state.SkipWithError("benchmark text is not valid UTF-8 once unmasked");
    return;
  }
  ApplyMask(std::span(masked), kMaskingKey);

  std::size_t copyIdx = 0;
  for ([[maybe_unused]] auto iter : state) {
    const std::span<std::byte> payload(copies.data() + (copyIdx * size), size);
    if constexpr (kUnmasking == Unmasking::TwoPasses) {
      ApplyMask(payload, kMaskingKey);
      benchmark::DoNotOptimize(IsValidUtf8(payload));
    } else if constexpr (kUnmasking == Unmasking::SinglePass) {
      benchmark::DoNotOptimize(UnmaskAndValidateUtf8(payload, kMaskingKey));
    }
    std::memcpy(payload.data(), masked.data(), size);
    benchmark::ClobberMemory();
    copyIdx = copyIdx + 1 == nbCopies ? 0 : copyIdx + 1;
  }

  state.SetBytesProcessed(static_cast<int64_t>(state.iterations()) * static_cast<int64_t>(size));
  state.SetLabel(std::string(kSampleNames[sampleIdx]));
}

void UnmaskArgs(benchmark::Benchmark* bench) {
  bench->ArgNames({"text", "size"})->ArgsProduct({{0, 2, 4}, {16, 40, 100, 256, 4096, 65536}});
}

}  // namespace

BENCHMARK(BM_Utf8<PreviousValidateUtf8>)->Name("Previous")->Apply(Args);
BENCHMARK(BM_Utf8<IsValidUtf8>)->Name("IsValidUtf8")->Apply(Args);
BENCHMARK(BM_Utf8<detail::IsValidUtf8Scalar>)->Name("Scalar")->Apply(Args);
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
BENCHMARK(BM_Utf8<detail::IsValidUtf8Ssse3, Isa::Ssse3>)->Name("Ssse3")->Apply(Args);
BENCHMARK(BM_Utf8<detail::IsValidUtf8Avx2, Isa::Avx2>)->Name("Avx2")->Apply(Args);
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
BENCHMARK(BM_Utf8<detail::IsValidUtf8Neon>)->Name("Neon")->Apply(Args);
#endif
BENCHMARK(BM_Unmask<Unmasking::RestoreOnly>)->Name("UnmaskRestoreOnly")->Apply(UnmaskArgs);
BENCHMARK(BM_Unmask<Unmasking::TwoPasses>)->Name("ApplyMaskThenIsValidUtf8")->Apply(UnmaskArgs);
BENCHMARK(BM_Unmask<Unmasking::SinglePass>)->Name("UnmaskAndValidateUtf8")->Apply(UnmaskArgs);

BENCHMARK_MAIN();
