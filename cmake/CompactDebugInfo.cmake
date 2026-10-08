# AERONET_COMPACT_DEBUG_INFO: split DWARF + compressed debug sections.
#
# Test executables link the aeronet libraries statically, so the debug info of the whole library used to be
# copied into each of them (~80 MB per test in Debug, ~200 MB with ASAN), and editing one library source
# rewrote gigabytes of binaries. With -gsplit-dwarf the bulk of the debug info stays in one .dwo file per
# object (written once, never copied by the linker), and the remaining debug sections are compressed
# (zstd, or zlib when zstd is not available). gdb, lldb and llvm-symbolizer read the .dwo files from the
# build tree, so nothing is lost for debugging, as long as the build tree is not moved.
#
# The options are added for the whole directory tree, dependencies included (gtest and the other static
# dependencies are linked into each test as well), for C++ only: C dependencies have little debug info, and
# the C compiler may differ from the C++ one.

function(_aeronet_setup_compact_debug_info)
  if(APPLE OR WIN32 OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # Mach-O already leaves the debug info in the object files, and MSVC in .pdb files.
    message(STATUS "[aeronet] AERONET_COMPACT_DEBUG_INFO has no effect with this platform / compiler")
    return()
  endif()

  include(CheckCXXCompilerFlag)
  include(CheckLinkerFlag)

  check_cxx_compiler_flag(-gsplit-dwarf AERONET_CXX_SUPPORTS_SPLIT_DWARF)
  if(NOT AERONET_CXX_SUPPORTS_SPLIT_DWARF)
    message(STATUS "[aeronet] AERONET_COMPACT_DEBUG_INFO: -gsplit-dwarf is not supported, option ignored")
    return()
  endif()

  # Compressed objects must be readable by the linker, which must also be able to compress its output.
  # -Werror: Clang only warns when it was built without the requested compression library.
  foreach(_gz IN ITEMS zstd zlib)
    set(CMAKE_REQUIRED_FLAGS "-g -gz=${_gz} -Werror")
    check_cxx_compiler_flag(-gz=${_gz} AERONET_CXX_SUPPORTS_GZ_${_gz})
    if(AERONET_CXX_SUPPORTS_GZ_${_gz})
      check_linker_flag(CXX -gz=${_gz} AERONET_LINKER_SUPPORTS_GZ_${_gz})
      if(AERONET_LINKER_SUPPORTS_GZ_${_gz})
        set(_gz_flag -gz=${_gz})
        break()
      endif()
    endif()
  endforeach()

  # Without compression, split DWARF alone does not pay off: the linked binaries keep the (uncompressed)
  # line tables and the pubnames sections emitted for split DWARF, and end up as large as before.
  if(NOT _gz_flag)
    message(STATUS "[aeronet] AERONET_COMPACT_DEBUG_INFO: debug section compression is not supported, option ignored")
    return()
  endif()

  message(STATUS "[aeronet] Compact debug info: -gsplit-dwarf ${_gz_flag}")
  add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-gsplit-dwarf;${_gz_flag}>")
  add_link_options("$<$<LINK_LANGUAGE:CXX>:${_gz_flag}>")
endfunction()

if(AERONET_COMPACT_DEBUG_INFO)
  _aeronet_setup_compact_debug_info()
endif()
