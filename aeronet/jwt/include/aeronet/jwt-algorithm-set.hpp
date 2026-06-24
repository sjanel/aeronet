#pragma once

#include <cstdint>
#include <initializer_list>

#include "aeronet/jwt-algorithm.hpp"

namespace aeronet {

// A compact set of JwtAlgorithm values backed by a single 16-bit mask (there are only 13
// algorithms). Used by JwtVerifyOptions to pin the accepted "alg" values without any allocation
// or dangling-span lifetime hazard. An empty set means "any supported algorithm" (never "none").
class JwtAlgorithmSet {
 public:
  constexpr JwtAlgorithmSet() noexcept = default;

  constexpr JwtAlgorithmSet(std::initializer_list<JwtAlgorithm> algs) noexcept {
    for (JwtAlgorithm alg : algs) {
      add(alg);
    }
  }

  constexpr JwtAlgorithmSet& add(JwtAlgorithm alg) noexcept {
    _bits |= Bit(alg);
    return *this;
  }

  [[nodiscard]] constexpr bool contains(JwtAlgorithm alg) const noexcept { return (_bits & Bit(alg)) != 0; }

  [[nodiscard]] constexpr bool empty() const noexcept { return _bits == 0; }

 private:
  static constexpr uint16_t Bit(JwtAlgorithm alg) noexcept {
    return static_cast<uint16_t>(1U << static_cast<unsigned>(alg));
  }

  uint16_t _bits{};
};

}  // namespace aeronet
