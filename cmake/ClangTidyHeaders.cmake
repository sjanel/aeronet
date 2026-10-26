## cmake/ClangTidyHeaders.cmake
# Run clang-tidy on every aeronet header, as the *main file* of its own clang-tidy invocation.
#
# CXX_CLANG_TIDY (see SetProjectProperties.cmake) only analyzes translation units. Checks that report on the main file
# only, like misc-include-cleaner (missing / unused includes), never see a header reached through a .cpp, and the
# diagnostics of the other checks inside a header are dropped by HeaderFilterRegex in .clang-tidy (on purpose: each
# header would otherwise be reported once per translation unit that includes it).
#
# One custom command per header, with a stamp file as output: ninja runs them in parallel and re-runs only the headers
# that changed. Note that a header is not re-analyzed when one of the headers it includes changes. A header with a
# finding does not get its stamp when AERONET_WARNINGS_AS_ERRORS is on, so that it fails the build until it is fixed.
#
# clang-tidy deduces the flags of a header from the closest entry of compile_commands.json. The include directories of
# all the modules are added on top of that, as some directories have no translation unit with the right -I flags.
# The headers of the tests (test_support and the test directories) are only analyzed with AERONET_BUILD_TESTS: without
# any translation unit of theirs in compile_commands.json, they would get the flags of another module.

# Headers that are not meant to be compiled alone.
set(_aeronet_tidy_header_excludes
    aeronet/tech/include/aeronet/internal/city_impl.hpp # fragment of city.hpp
)

file(GLOB_RECURSE _aeronet_tidy_headers CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/aeronet/*.hpp")
file(GLOB _aeronet_tidy_include_dirs LIST_DIRECTORIES true
     "${PROJECT_SOURCE_DIR}/aeronet/*/include"
     "${PROJECT_SOURCE_DIR}/aeronet/*/*/include")

set(_aeronet_tidy_args -p "${CMAKE_BINARY_DIR}" --quiet --extra-arg-before=--driver-mode=g++)
if(AERONET_WARNINGS_AS_ERRORS)
  list(APPEND _aeronet_tidy_args --warnings-as-errors=*)
endif()
foreach(_dir IN LISTS _aeronet_tidy_include_dirs)
  if(IS_DIRECTORY "${_dir}")
    list(APPEND _aeronet_tidy_args "--extra-arg=-I${_dir}")
  endif()
endforeach()

set(_aeronet_tidy_stamps)
foreach(_header IN LISTS _aeronet_tidy_headers)
  file(RELATIVE_PATH _rel "${PROJECT_SOURCE_DIR}" "${_header}")
  if(_rel IN_LIST _aeronet_tidy_header_excludes)
    continue()
  endif()
  if(NOT AERONET_BUILD_TESTS AND _rel MATCHES "^aeronet/(test_support/|[^/]+/test/)")
    continue()
  endif()
  set(_stamp "${CMAKE_BINARY_DIR}/clang-tidy-headers/${_rel}.stamp")
  add_custom_command(
    OUTPUT "${_stamp}"
    COMMAND "${CLANG_TIDY}" ${_aeronet_tidy_args} "${_header}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_stamp}"
    DEPENDS "${_header}" "${PROJECT_SOURCE_DIR}/.clang-tidy"
    COMMENT "clang-tidy ${_rel}"
    VERBATIM)
  list(APPEND _aeronet_tidy_stamps "${_stamp}")
endforeach()

add_custom_target(aeronet-clang-tidy-headers ALL DEPENDS ${_aeronet_tidy_stamps})
