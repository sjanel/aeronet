include(FetchContent)

# Sets out_var to TRUE when a package manager (vcpkg, Conan) provides the dependencies: the consumers of aeronet link
# its packages, so aeronet must be built with them, never with a downloaded copy. Conan defines no variable of its
# own: it is recognized by the name of the toolchain file it generates.
function(aeronet_package_manager_used out_var)
  if(DEFINED VCPKG_TOOLCHAIN OR CMAKE_TOOLCHAIN_FILE MATCHES "conan_toolchain\\.cmake$")
    set(${out_var} TRUE PARENT_SCOPE)
  else()
    set(${out_var} FALSE PARENT_SCOPE)
  endif()
endfunction()

# Decide whether system packages are allowed for this configuration.
# Policy:
#  - Package manager      -> System packages allowed (full multi-config support)
#  - Single-config Debug  -> FetchContent
#  - Multi-config         -> FetchContent
#  - Release              -> System packages allowed
function(aeronet_system_packages_allowed out_var)
  aeronet_package_manager_used(_package_manager)
  if(_package_manager)
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

    # EXCLUDE_FROM_ALL: build only the libraries of the dependency that aeronet links, not its other libraries,
    # tools or tests.
    FetchContent_Declare(${AFOF_NAME} ${AFOF_DECLARE} EXCLUDE_FROM_ALL)

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

# Adds compile (and, for linked targets, link) options to all the targets built in a directory, subdirectories
# included - typically a dependency populated by FetchContent.
function(aeronet_add_options_to_dir_targets dir)
  get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_target IN LISTS _targets)
    get_target_property(_type ${_target} TYPE)
    if(_type MATCHES "^(STATIC_LIBRARY|OBJECT_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|EXECUTABLE)$")
      target_compile_options(${_target} PRIVATE ${ARGN})
    endif()
    if(_type MATCHES "^(SHARED_LIBRARY|MODULE_LIBRARY|EXECUTABLE)$")
      target_link_options(${_target} PRIVATE ${ARGN})
    endif()
  endforeach()
  get_property(_subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(_subdir IN LISTS _subdirs)
    aeronet_add_options_to_dir_targets("${_subdir}" ${ARGN})
  endforeach()
endfunction()
