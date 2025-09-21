#pragma once

#include <amc/allocator.hpp>
#include <functional>

#include "fixedcapacityvector.hpp"
#include "flatset.hpp"
#include "http-method.hpp"

namespace aeronet::http {

using MethodSet = FlatSet<Method, std::less<>, amc::vec::EmptyAlloc, FixedCapacityVector<Method, 9>>;

}  // namespace aeronet::http