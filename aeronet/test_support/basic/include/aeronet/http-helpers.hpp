#pragma once

#include <string_view>

#include "aeronet/raw-chars.hpp"

namespace aeronet {

RawChars MakeHttp1HeaderLine(std::string_view name, std::string_view value, bool withCRLF = true);

RawChars BuildRawHttp11(std::string_view method, std::string_view target, std::string_view extraHeaders = {},
                        std::string_view body = {});

}  // namespace aeronet