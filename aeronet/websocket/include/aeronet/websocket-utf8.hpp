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
