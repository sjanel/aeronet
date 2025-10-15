file(READ "${CMAKE_CURRENT_LIST_DIR}/../../VERSION" AERONET_VCPKG_VERSION_RAW)
string(STRIP "${AERONET_VCPKG_VERSION_RAW}" AERONET_VCPKG_VERSION)
set(AERONET_VCPKG_TAG "v${AERONET_VCPKG_VERSION}")

if(DEFINED ENV{AERONET_VCPKG_LOCAL})
    set(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../..")
else()
    execute_process(
        COMMAND git ls-remote --tags https://github.com/sjanel/aeronet.git ${AERONET_VCPKG_TAG}
        OUTPUT_VARIABLE AERONET_TAG_QUERY
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(AERONET_TAG_QUERY STREQUAL "")
        set(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../..")
    else()
        string(REPLACE "\n" ";" AERONET_TAG_LINES "${AERONET_TAG_QUERY}")
        list(GET AERONET_TAG_LINES -1 AERONET_TAG_LAST)
        string(REGEX MATCH "^[0-9a-fA-F]+" AERONET_TAG_COMMIT "${AERONET_TAG_LAST}")
        if(AERONET_TAG_COMMIT)
            vcpkg_from_git(OUT_SOURCE_PATH SOURCE_PATH URL https://github.com/sjanel/aeronet.git REF ${AERONET_TAG_COMMIT})
        else()
            set(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../..")
        endif()
    endif()
endif()

# Map enabled features to CMake options
set(AERONET_FEATURE_OPTS)
foreach(f OPENSSL SPDLOG BROTLI ZLIB ZSTD OPENTELEMETRY)
    string(TOUPPER ${f} _F)
    if("${f}" IN_LIST FEATURES)
        list(APPEND AERONET_FEATURE_OPTS -DAERONET_ENABLE_${_F}=ON)
    else()
        list(APPEND AERONET_FEATURE_OPTS -DAERONET_ENABLE_${_F}=OFF)
    endif()
endforeach()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    set(AERONET_BUILD_SHARED ON)
else()
    set(AERONET_BUILD_SHARED OFF)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        -DAERONET_BUILD_TESTS=OFF
        -DAERONET_BUILD_EXAMPLES=OFF
        -DAERONET_BUILD_BENCHMARKS=OFF
        -DAERONET_INSTALL=ON
        -DCMAKE_CXX_STANDARD=23
        -DAERONET_BUILD_SHARED=${AERONET_BUILD_SHARED}
        ${AERONET_FEATURE_OPTS}
)

vcpkg_cmake_install()

# Copy license
file(INSTALL ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/aeronet RENAME copyright)

# Fix up generated CMake config and ensure consumers can find targets in share/
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/aeronet)
file(MAKE_DIRECTORY ${CURRENT_PACKAGES_DIR}/share/aeronet)
if(EXISTS "${CURRENT_PACKAGES_DIR}/lib/cmake/aeronet")
    file(GLOB _cmake_files RELATIVE "${CURRENT_PACKAGES_DIR}/lib/cmake/aeronet" "${CURRENT_PACKAGES_DIR}/lib/cmake/aeronet/*")
    foreach(f ${_cmake_files})
        file(COPY "${CURRENT_PACKAGES_DIR}/lib/cmake/aeronet/${f}" DESTINATION "${CURRENT_PACKAGES_DIR}/share/aeronet")
    endforeach()
endif()

# Minimal fallback: if no exported targets file exists, create a trivial
# IMPORTED-targets file under share so older consumer layout expectations
# succeed. Keep it intentionally small and conservative.
if(NOT EXISTS "${CURRENT_PACKAGES_DIR}/lib/cmake/aeronet/aeronetTargets.cmake" AND NOT EXISTS "${CURRENT_PACKAGES_DIR}/share/aeronet/aeronetTargets.cmake")
    file(WRITE "${CURRENT_PACKAGES_DIR}/share/aeronet/aeronetTargets.cmake" "# vcpkg-generated fallback aeronetTargets.cmake\nset(_AERONET_PREFIX \"${CMAKE_CURRENT_LIST_DIR}/../..\")\n\n")
    file(APPEND "${CURRENT_PACKAGES_DIR}/share/aeronet/aeronetTargets.cmake" "function(_aeronet_add_imported name libname)\n  if(NOT TARGET aeronet::${name})\n    add_library(aeronet::${name} UNKNOWN IMPORTED)\n    set_target_properties(aeronet::${name} PROPERTIES IMPORTED_LOCATION \"${_AERONET_PREFIX}/lib/lib${libname}.a\" INTERFACE_INCLUDE_DIRECTORIES \"${_AERONET_PREFIX}/include\")\n  endif()\nendfunction()\n\n")
    file(APPEND "${CURRENT_PACKAGES_DIR}/share/aeronet/aeronetTargets.cmake" "_aeronet_add_imported(aeronet aeronet)\n_aeronet_add_imported(aeronet_http aeronet_http)\n_aeronet_add_imported(aeronet_tech aeronet_tech)\n_aeronet_add_imported(aeronet_objects aeronet_objects)\n_aeronet_add_imported(aeronet_sys aeronet_sys)\n")
endif()

# Remove debug include duplication if any
file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug/include)

