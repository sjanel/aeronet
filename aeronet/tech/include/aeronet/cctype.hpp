#pragma once

#include <cctype>

namespace aeronet {

/// Safe std::isalnum version. See https://en.cppreference.com/w/cpp/string/byte/isalnum
inline bool isalnum(char ch) { return std::isalnum(static_cast<unsigned char>(ch)) != 0; }

/// Safe std::isalpha version. See https://en.cppreference.com/w/cpp/string/byte/isalpha
inline bool isalpha(char ch) { return std::isalpha(static_cast<unsigned char>(ch)) != 0; }

/// Safe std::isblank version. See https://en.cppreference.com/w/cpp/string/byte/isblank
inline bool isblank(char ch) { return std::isblank(static_cast<unsigned char>(ch)) != 0; }

/// Safe std::isdigit version. See https://en.cppreference.com/w/cpp/string/byte/isdigit
inline bool isdigit(char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; }

/// Safe std::islower version. See https://en.cppreference.com/w/cpp/string/byte/islower
inline bool islower(char ch) { return std::islower(static_cast<unsigned char>(ch)) != 0; }

/// Safe std::isspace version. See https://en.cppreference.com/w/cpp/string/byte/isspace
inline bool isspace(char ch) { return std::isspace(static_cast<unsigned char>(ch)) != 0; }

}  // namespace aeronet