#pragma once

#include <cstdint>

namespace aeronet {

enum class KtlsEnableResult : std::uint8_t { Unknown, Unsupported, Enabled, Disabled };

}