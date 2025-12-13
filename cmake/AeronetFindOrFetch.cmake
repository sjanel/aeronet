include(FetchContent)

# Decide whether system packages are allowed for this configuration.
# Policy:
#  - Single-config Debug  -> FetchContent
#  - Multi-config         -> FetchContent
#  - Release              -> System packages allowed
function(aeronet_system_packages_allowed out_var)
  # vcpkg / Conan → full multi-config support
  if(DEFINED VCPKG_TOOLCHAIN
     OR DEFINED CONAN_TOOLCHAIN_FILE
     OR DEFINED CONAN_CMAKE_TOOLCHAIN_FILE)
    set(${out_var} TRUE PARENT_SCOPE)
    return()
  endif()

  set(_allowed TRUE)

  # Multi-config generators without a package manager
  if(DEFINED CMAKE_CONFIGURATION_TYPES)
    set(_allowed FALSE)
  endif()

  # Single-config Debug without a package manager
  if(DEFINED CMAKE_BUILD_TYPE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_allowed FALSE)
  endif()

  set(${out_var} ${_allowed} PARENT_SCOPE)
endfunction()


# Declare a dependency via find_package() or FetchContent_Declare().
# Population is deferred.
#
# Required arguments:
#   NAME        - logical dependency name (FetchContent name)
#   TARGETS     - targets that must exist if system package is usable
#
# Optional:
#   CONFIG      - use find_package(CONFIG)
#   DECLARE     - FetchContent_Declare() arguments
#
function(aeronet_find_or_declare)
  set(options CONFIG)
  set(oneValueArgs NAME)
  set(multiValueArgs TARGETS DECLARE)
  cmake_parse_arguments(AFOF "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT AFOF_NAME)
    message(FATAL_ERROR "aeronet_find_or_declare: NAME is required")
  endif()

  aeronet_system_packages_allowed(_use_system)

  # Try system package
  if(_use_system)
    if(AFOF_CONFIG)
      find_package(${AFOF_NAME} CONFIG QUIET)
    else()
      find_package(${AFOF_NAME} QUIET)
    endif()
  endif()

  # Verify required targets exist
  set(_usable TRUE)
  foreach(t IN LISTS AFOF_TARGETS)
    if(NOT TARGET ${t})
      set(_usable FALSE)
      break()
    endif()
  endforeach()

  # Fallback: declare FetchContent but do NOT populate yet
  if(NOT _usable)
    if(NOT AFOF_DECLARE)
      message(FATAL_ERROR
        "Package ${AFOF_NAME} not found or unusable and no FetchContent declaration provided")
    endif()

    FetchContent_Declare(${AFOF_NAME} ${AFOF_DECLARE})

    # Append only once
    list(FIND AeronetFetchContentPackagesToMakeAvailable ${AFOF_NAME} _idx)
    if(_idx EQUAL -1)
      list(APPEND AeronetFetchContentPackagesToMakeAvailable ${AFOF_NAME})
      set(AeronetFetchContentPackagesToMakeAvailable
          "${AeronetFetchContentPackagesToMakeAvailable}"
          PARENT_SCOPE)
    endif()
  endif()
endfunction()
