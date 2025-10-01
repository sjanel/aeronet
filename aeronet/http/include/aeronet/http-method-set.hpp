#pragma once

#include <amc/allocator.hpp>
#include <functional>

#include "aeronet/http-method.hpp"
#include "fixedcapacityvector.hpp"
#include "flatset.hpp"

namespace aeronet::http {

using MethodSet = FlatSet<Method, std::less<>, amc::vec::EmptyAlloc, FixedCapacityVector<Method, 9>>;

}  // namespace aeronet::http