function(set_project_properties name)
  # Warning levels
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Basic warnings
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
      if (AERONET_WARNINGS_AS_ERRORS)
          target_compile_options(${name} PRIVATE -Werror)
      endif()
    endif()
  else()
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
      target_compile_options(${name} PRIVATE -Wall -Wdisabled-optimization)
    endif()
  endif()
  # Address/UB sanitizers (activated via AERONET_ENABLE_ASAN)
  if(AERONET_ENABLE_ASAN)
    target_compile_options(${name} PRIVATE ${AERONET_ASAN_OPTIONS})
    target_link_options(${name} PRIVATE ${AERONET_ASAN_OPTIONS})
  endif()

  if(AERONET_ENABLE_CLANG_TIDY AND CLANG_TIDY)
    set_target_properties(${name} PROPERTIES
      CXX_CLANG_TIDY "clang-tidy;--extra-arg-before=--driver-mode=g++"
    )
  endif()
  if (LINK_AMC)
    target_link_libraries(${name} PRIVATE amc::amc)
  endif()
  if(AERONET_ENABLE_SPDLOG)
    # Use header-only mode to avoid introducing a link dependency that would
    # force exporting/packaging spdlog. Consumers can supply their own spdlog
    # if they also define AERONET_ENABLE_SPDLOG, otherwise they build without it.
    target_compile_definitions(${name} PUBLIC AERONET_ENABLE_SPDLOG SPDLOG_USE_STD_FORMAT)
    # Do NOT link the spdlog target (even privately) or the install(EXPORT ...) step
    # will complain that the exported aeronet targets require a target not in any
    # export set. Instead, harvest its include directories (header-only) so builds succeed.
    if(TARGET spdlog::spdlog)
      get_target_property(_spdlog_includes spdlog::spdlog INTERFACE_INCLUDE_DIRECTORIES)
      if(_spdlog_includes)
        target_include_directories(${name} PRIVATE ${_spdlog_includes})
      endif()
    elseif(TARGET spdlog)
      get_target_property(_spdlog_includes spdlog INTERFACE_INCLUDE_DIRECTORIES)
      if(_spdlog_includes)
        target_include_directories(${name} PRIVATE ${_spdlog_includes})
      endif()
    endif()
  endif()
  if(AERONET_ENABLE_OPENSSL)
    # Expose compile definition so core can gate TLS logic, but do not link OpenSSL here.
    # Only the dedicated aeronet_tls module should link against OpenSSL to contain dependency surface.
    target_compile_definitions(${name} PUBLIC AERONET_ENABLE_OPENSSL)
  endif()
  if(AERONET_ENABLE_ZLIB)
    target_compile_definitions(${name} PUBLIC AERONET_ENABLE_ZLIB)
    # We do not link zlib to every target now; a future dedicated compression TU / adapter will
    # link privately to ZLIB::ZLIB (or zlib) where needed. Expose includes if the imported target provides them.
    if(TARGET ZLIB::ZLIB)
      target_link_libraries(${name} PRIVATE ZLIB::ZLIB)
      get_target_property(_zlib_includes ZLIB::ZLIB INTERFACE_INCLUDE_DIRECTORIES)
      if(_zlib_includes)
        target_include_directories(${name} PRIVATE ${_zlib_includes})
      endif()
    endif()
  endif()
  if(AERONET_ENABLE_ZSTD)
    target_compile_definitions(${name} PUBLIC AERONET_ENABLE_ZSTD)
    # Detect whichever zstd target name is available in this build context.
    set(_AERONET_ZSTD_TARGET "")
    if(TARGET libzstd_static)
      set(_AERONET_ZSTD_TARGET libzstd_static)
    elseif(TARGET zstd::libzstd)
      set(_AERONET_ZSTD_TARGET zstd::libzstd)
    elseif(TARGET zstd::zstd)
      set(_AERONET_ZSTD_TARGET zstd::zstd)
    elseif(TARGET ZSTD::ZSTD)
      set(_AERONET_ZSTD_TARGET ZSTD::ZSTD)
    endif()
    if(_AERONET_ZSTD_TARGET)
      # Keep PRIVATE to signal implementation detail; for static libs some CMake versions may still
      # surface it transitively when exporting. If stricter encapsulation is desired we would need
      # to vendor or wrap symbols.
      target_link_libraries(${name} PRIVATE ${_AERONET_ZSTD_TARGET})
      get_target_property(_zstd_includes ${_AERONET_ZSTD_TARGET} INTERFACE_INCLUDE_DIRECTORIES)
      if(_zstd_includes)
        target_include_directories(${name} PRIVATE ${_zstd_includes})
      endif()
    endif()
  endif()
  target_compile_definitions(${name} PUBLIC AERONET_PROJECT_VERSION="${AERONET_PROJECT_VERSION}")
endfunction()

function(add_project_executable name)
  add_executable(${name} ${ARGN})
  set_project_properties(${name})
endfunction()

function(add_project_library name)
  if(AERONET_BUILD_SHARED)
    add_library(${name} SHARED ${ARGN})
  else()
    add_library(${name} STATIC ${ARGN})
  endif()
  set_project_properties(${name})
endfunction()