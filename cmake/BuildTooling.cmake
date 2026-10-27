# Build tooling selected by the AERONET_ENABLE_* options: ccache, clang-tidy and the sanitizer flags.
# Included before the targets and the dependencies are created.

if(AERONET_ENABLE_CCACHE)
  find_program(CCACHE_PROGRAM ccache)
  if(CCACHE_PROGRAM)
    set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CCACHE_PROGRAM}")
  else()
    message(WARNING "ccache wanted but not found in PATH")
  endif()
endif()

# Another clang-tidy than the one in PATH is selected with -D CLANG_TIDY=/usr/bin/clang-tidy-23
if(AERONET_ENABLE_CLANG_TIDY)
  find_program(CLANG_TIDY "clang-tidy")
  if(NOT CLANG_TIDY)
    message(FATAL_ERROR "clang-tidy executable cannot be found")
  endif()
  message(STATUS "Activate clang-tidy: ${CLANG_TIDY}")
  # cmake/ClangTidyHeaders.cmake needs the compile database to analyze headers.
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# The flags can be overridden with -D AERONET_ASAN_OPTIONS=... / -D AERONET_TSAN_OPTIONS=...
if(AERONET_ENABLE_ASAN)
  if(NOT AERONET_ASAN_OPTIONS)
    set(AERONET_ASAN_OPTIONS -fsanitize=address -fsanitize=undefined -fsanitize=float-divide-by-zero -fno-sanitize-recover)
  endif()
  message(STATUS "Activate asan with options ${AERONET_ASAN_OPTIONS}")
endif()

if(AERONET_ENABLE_TSAN)
  if(NOT AERONET_TSAN_OPTIONS)
    set(AERONET_TSAN_OPTIONS -fsanitize=thread -fno-omit-frame-pointer)
  endif()
  message(STATUS "Activate tsan with options ${AERONET_TSAN_OPTIONS}")
endif()
