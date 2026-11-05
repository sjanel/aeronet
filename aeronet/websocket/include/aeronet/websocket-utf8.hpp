#pragma once

#include <cstddef>
#include <span>

#include "aeronet/websocket-constants.hpp"

namespace aeronet::websocket {

/// Tells whether data is well-formed UTF-8 (RFC 3629): no overlong encoding, no surrogate (U+D800 - U+DFFF), no
/// code point above U+10FFFF and no truncated sequence.
/// Runs the fastest kernel the CPU supports: AVX2 or SSSE3 on x86 (detected at runtime), NEON on ARM, scalar elsewhere.
[[nodiscard]] bool IsValidUtf8(std::span<const std::byte> data) noexcept;

/// Unmasks in place data, a frame payload masked with maskingKey (as ApplyMask()), and tells whether the unmasked data
/// is well-formed UTF-8 (as IsValidUtf8()), in a single pass over the data with the SIMD kernels.
/// The data is entirely unmasked whatever the result.
[[nodiscard]] bool UnmaskAndValidateUtf8(std::span<std::byte> data, MaskingKey maskingKey) noexcept;

/// ValidateUtf8Prefix() result for data that no continuation can make well-formed UTF-8.
inline constexpr std::size_t kInvalidUtf8Prefix = ~std::size_t{0};

/// Validates data as the start of a UTF-8 text that may end in the middle of a code point, like a fragment of a text
/// message that the next fragments complete. Returns the number of bytes (0 to 3) of its trailing incomplete sequence,
/// a valid start of a code point, or kInvalidUtf8Prefix when no continuation can make data well-formed (see
/// IsValidUtf8()).
[[nodiscard]] std::size_t ValidateUtf8Prefix(std::span<const std::byte> data) noexcept;

namespace detail {

// The kernels behind IsValidUtf8 and UnmaskAndValidateUtf8, exposed for tests and benchmarks.
// A SIMD kernel may only be called on a CPU supporting its instruction set (see HasSsse3 / HasAvx2).

[[nodiscard]] bool IsValidUtf8Scalar(std::span<const std::byte> data) noexcept;

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
[[nodiscard]] bool IsValidUtf8Ssse3(std::span<const std::byte> data) noexcept;
[[nodiscard]] bool IsValidUtf8Avx2(std::span<const std::byte> data) noexcept;
[[nodiscard]] bool UnmaskAndValidateUtf8Ssse3(std::span<std::byte> data, MaskingKey maskingKey) noexcept;
[[nodiscard]] bool UnmaskAndValidateUtf8Avx2(std::span<std::byte> data, MaskingKey maskingKey) noexcept;
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
[[nodiscard]] bool IsValidUtf8Neon(std::span<const std::byte> data) noexcept;
[[nodiscard]] bool UnmaskAndValidateUtf8Neon(std::span<std::byte> data, MaskingKey maskingKey) noexcept;
#endif

}  // namespace detail

}  // namespace aeronet::websocket
