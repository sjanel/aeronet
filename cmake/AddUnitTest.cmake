# ---------------------------------------------------------------------------
# Windows runtime DLL path injection for CTest
# ---------------------------------------------------------------------------
# When Ninja + MSVC is used outside a VS Developer Command Prompt the vcpkg
# applocal.ps1 post-build step fails silently (dumpbin not found), so test
# executables cannot locate their shared-library dependencies at runtime
# (exit code 0xc0000135 = STATUS_DLL_NOT_FOUND).  This helper function
# prepends the required DLL directories to the PATH of every CTest test.
function(_aeronet_inject_dll_path test_name)
  if(NOT WIN32)
    return()
  endif()
  set(_dirs "")
  # vcpkg shared libraries: protobuf, CURL, abseil, brotli, spdlog, etc.
  if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
      list(APPEND _dirs "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin")
    else()
      list(APPEND _dirs "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    endif()
  endif()
  # System OpenSSL: OPENSSL_INCLUDE_DIR = <root>/include → DLLs live in <root>/bin
  if(DEFINED OPENSSL_INCLUDE_DIR AND OPENSSL_INCLUDE_DIR)
    get_filename_component(_ssl_root "${OPENSSL_INCLUDE_DIR}" DIRECTORY)
    if(EXISTS "${_ssl_root}/bin")
      list(APPEND _dirs "${_ssl_root}/bin")
    endif()
  endif()
  list(REMOVE_DUPLICATES _dirs)
  foreach(_dir IN LISTS _dirs)
    if(EXISTS "${_dir}")
      set_property(TEST "${test_name}" APPEND PROPERTY ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:${_dir}")
    endif()
  endforeach()
endfunction()
# ---------------------------------------------------------------------------

#[[ Create an executable
Syntax:
AeronetAddExecutable(<name> src1 [src2 ...] [LIBRARIES lib1 lib2 ...] [DEFINITIONS def1 def2])
will compile an executable named <name> from source files src1 src2...
with pre-processor definitions def1 def2 (-Ddef1 -Ddef2 ... will be added to compile command)
and link against lib1 lib2 ...and libm

Examples:
AeronetAddExecutable(myexe src1.cpp)
AeronetAddExecutable(myexe src1.cpp
   LIBRARIES ${CMAKE_SOURCE_DIR}/myLib
   DEFINITIONS UNIT_TEST)
#]]
function(AeronetAddExecutable name)
  set(cur_var "sources")
  set(exe_sources "")
  set(exe_libraries "")
  set(exe_directories "")
  set(exe_include_dirs "")

  foreach(arg IN LISTS ARGN)
    if(arg STREQUAL "LIBRARIES")
      set(cur_var "libraries")
    elseif(arg STREQUAL "DIRECTORIES")
      set(cur_var "directories")
    else()
      list(APPEND exe_${cur_var} ${arg})

      if(cur_var STREQUAL "sources")
        get_filename_component(src_dir ${arg} DIRECTORY)
        list(APPEND exe_include_dirs ${src_dir})
      endif()
    endif()
  endforeach()

  AeronetAddProjectExecutable(${name} ${exe_sources})
  set_target_properties(${name} PROPERTIES
    BUILD_RPATH "${runtime_path}")
  if(exe_libraries)
    list(REMOVE_DUPLICATES exe_libraries)
  endif()
  target_link_libraries(${name} PRIVATE ${exe_libraries})
  if(exe_include_dirs)
    list(REMOVE_DUPLICATES exe_include_dirs)
  endif()
  target_include_directories(${name} PRIVATE ${exe_include_dirs} ${exe_directories})
endfunction()

#[[ Create a unit test
Syntax:
AeronetAddUnitTest(<name> src1 [src2 ...] [LIBRARIES lib1 lib2 ...] [DEFINITIONS def1 def2])
will compile an unit test named <name> from source files src1 src2...
with pre-processor definitions def1 def2 (-Ddef1 -Ddef2 ... will be added to compile command)
and link against lib1 lib2 ... and libm

Examples:
AeronetAddUnitTest(myexe src1.cpp)
AeronetAddUnitTest(myexe src1.cpp DEFINITIONS UNIT_TEST)
#]]
function(AeronetAddUnitTest name)
  set(oneValueArgs)
  set(multiValueArgs)
  cmake_parse_arguments(PARSE_ARGV 1 MY "${options}" "${oneValueArgs}" "${multiValueArgs}")

  if(AERONET_BUILD_TESTS)
    AeronetAddExecutable(${name} ${MY_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE GTest::gmock_main)

    target_include_directories(${name} PRIVATE include)

    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
      ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1 abort_on_error=1 print_stacktrace=1;\
                          LSAN_OPTIONS=detect_leaks=1 malloc_context_size=2 print_suppressions=0"
      WORKING_DIRECTORY ${CMAKE_HOME_DIRECTORY})
    _aeronet_inject_dll_path(${name})
  endif()
endfunction()