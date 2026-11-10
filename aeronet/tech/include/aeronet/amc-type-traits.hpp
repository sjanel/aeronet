#pragma once

#include <amc/type_traits.hpp>  // IWYU pragma: export
#include <memory>
#include <type_traits>

// Relocation traits of standard types that amc does not declare. Included by all the amc container headers of aeronet
// (vector.hpp, small-vector.hpp, fixedcapacityvector.hpp, flat-set.hpp), so that every container of these types is
// built with the same trait. Include it instead of <amc/type_traits.hpp>: a use of amc::is_trivially_relocatable on one
// of these types before the specializations below instantiates the primary template (an error with GCC, a silent ODR
// violation with Clang).
// Each holds pointers that never point into the object itself: copying its bytes to new storage and forgetting the old
// ones is a valid move (libstdc++, libc++ and the MSVC STL).
namespace amc {

/// std::unique_ptr only holds its pointer and its deleter: trivially relocatable when they are (a raw pointer, and an
/// empty deleter or a function pointer).
template <class T, class Deleter>
struct is_trivially_relocatable<std::unique_ptr<T, Deleter>>
    : std::bool_constant<is_trivially_relocatable<typename std::unique_ptr<T, Deleter>::pointer>::value &&
                         is_trivially_relocatable<Deleter>::value> {};

/// std::shared_ptr holds a pointer to its object and one to its control block.
template <class T>
struct is_trivially_relocatable<std::shared_ptr<T>> : std::true_type {};

}  // namespace amc
