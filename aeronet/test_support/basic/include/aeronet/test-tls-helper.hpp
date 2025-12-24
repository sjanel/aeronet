#pragma once

#include <cstdint>
#include <string>
#include <utility>

namespace aeronet::test {

// Shared test utility for generating ephemeral self-signed certificates entirely in memory.
// Returns {certPem, keyPem}. Intended ONLY for tests – no persistence.
// Default is RSA 2048-bit, 1h validity.
enum class KeyAlgorithm : uint8_t { Rsa2048, EcdsaP256 };

std::pair<std::string, std::string> MakeEphemeralCertKey(const char* commonName = "localhost", int validSeconds = 3600,
                                                         KeyAlgorithm alg = KeyAlgorithm::Rsa2048);

}  // namespace aeronet::test
