#pragma once

#include <cstdint>

namespace aeronet {

enum class ZlibVariant : int8_t { uninitialized, gzip, deflate, raw };

}