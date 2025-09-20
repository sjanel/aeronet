#[[ Create a unit test
Syntax:
add_unit_test(<name> src1 [src2 ...] [LIBRARIES lib1 lib2 ...] [DEFINITIONS def1 def2])
will compile an unit test named <name> from source files src1 src2...
with pre-processor definitions def1 def2 (-Ddef1 -Ddef2 ... will be added to compile command)
and link against lib1 lib2 ... and libm

Examples:
add_unit_test(myexe src1.cpp)
add_unit_test(myexe src1.cpp DEFINITIONS UNIT_TEST)
#]]
function(add_unit_test name)
  set(oneValueArgs)
  set(multiValueArgs)
  cmake_parse_arguments(PARSE_ARGV 1 MY "${options}" "${oneValueArgs}" "${multiValueArgs}")

  if(AERONET_BUILD_TESTS)
    add_exe(${name} ${MY_UNPARSED_ARGUMENTS})
    target_link_libraries(${name} PRIVATE GTest::gtest GTest::gmock GTest::gmock_main)

    if(LINK_AMC)
      target_link_libraries(${name} PRIVATE amc::amc)
    endif()

    target_include_directories(${name} PRIVATE include)

    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
      ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1 abort_on_error=1 print_stacktrace=1;\
                          LSAN_OPTIONS=detect_leaks=1 malloc_context_size=2 print_suppressions=0"
      WORKING_DIRECTORY ${CMAKE_HOME_DIRECTORY})
  endif()
endfunction()

#[[ Create an executable
Syntax:
add_exe(<name> src1 [src2 ...] [LIBRARIES lib1 lib2 ...] [DEFINITIONS def1 def2])
will compile an executable named <name> from source files src1 src2...
with pre-processor definitions def1 def2 (-Ddef1 -Ddef2 ... will be added to compile command)
and link against lib1 lib2 ...and libm

Examples:
add_exe(myexe src1.cpp)
add_exe(myexe src1.cpp
   LIBRARIES ${CMAKE_SOURCE_DIR}/myLib
   DEFINITIONS UNIT_TEST)
#]]
function(add_exe name)
  set(cur_var "sources")
  set(exe_sources "")
  set(exe_libraries "")
  set(exe_definitions "")
  set(exe_directories "")
  set(exe_include_dirs "")

  foreach(arg IN LISTS ARGN)
    if(arg STREQUAL "LIBRARIES")
      set(cur_var "libraries")
    elseif(arg STREQUAL "DEFINITIONS")
      set(cur_var "definitions")
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

  add_project_executable(${name} ${exe_sources})
  set_target_properties(${name} PROPERTIES
    COMPILE_DEFINITIONS "${exe_definitions}"
    BUILD_RPATH "${runtime_path}")
  target_link_libraries(${name} PRIVATE ${exe_libraries})
  list(REMOVE_DUPLICATES exe_include_dirs)
  target_include_directories(${name} PRIVATE ${exe_include_dirs} ${exe_directories})
endfunction()