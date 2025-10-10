#pragma once

#include <string>
#include <utility>

namespace aeronet::test {

// Shared test utility for generating ephemeral self-signed RSA certificates entirely in memory.
// Returns {certPem, keyPem}. Intended ONLY for tests – no persistence, 2048-bit RSA, 1h validity.
std::pair<std::string, std::string> makeEphemeralCertKey(const char* commonName = "localhost", int validSeconds = 3600);

}  // namespace aeronet::test
