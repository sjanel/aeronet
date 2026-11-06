#include "aeronet/quality-value.hpp"

#include <cstddef>
#include <string_view>

#include "aeronet/cctype.hpp"

namespace aeronet::http {

QualityValue ParseQualityValue(std::string_view value) noexcept {
  // qvalue = ( "0" [ "." 0*3DIGIT ] ) / ( "1" [ "." 0*3("0") ] )
  if (value.empty() || (value[0] != '0' && value[0] != '1')) {
    return 0;
  }
  unsigned thousandths = value[0] == '1' ? kQualityValueMax : 0U;
  std::size_t pos = 1;
  if (pos < value.size() && value[pos] == '.') {
    ++pos;
    for (unsigned scale = 100U; pos < value.size() && isdigit(value[pos]); ++pos) {
      if (scale == 0) {
        return 0;  // more than 3 decimals
      }
      thousandths += static_cast<unsigned>(value[pos] - '0') * scale;
      scale /= 10U;
    }
  }
  // Above 1 ("1" followed by a non-zero decimal), or followed by something else than whitespace or a parameter.
  if (thousandths > kQualityValueMax ||
      (pos < value.size() && value[pos] != ' ' && value[pos] != '\t' && value[pos] != ';')) {
    return 0;
  }
  return static_cast<QualityValue>(thousandths);
}

}  // namespace aeronet::http
