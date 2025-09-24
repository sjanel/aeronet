# Derive tag from VERSION file (expects tags like v<version>)
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../VERSION" AERONET_VCPKG_VERSION_RAW)
string(STRIP "${AERONET_VCPKG_VERSION_RAW}" AERONET_VCPKG_VERSION)
set(AERONET_VCPKG_TAG "v${AERONET_VCPKG_VERSION}")

if(DEFINED ENV{AERONET_VCPKG_LOCAL})
    message(STATUS "[aeronet] Using local source tree for version ${AERONET_VCPKG_VERSION} (AERONET_VCPKG_LOCAL set)")
    set(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../..")
else()
    # Probe remote to see if the expected tag exists; if missing, fallback to local tree.
    execute_process(
        COMMAND git ls-remote --tags https://github.com/sjanel/aeronet.git ${AERONET_VCPKG_TAG}
        OUTPUT_VARIABLE AERONET_TAG_QUERY
        ERROR_VARIABLE AERONET_TAG_QUERY_ERR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(AERONET_TAG_QUERY STREQUAL "")
        message(WARNING "[aeronet] Tag ${AERONET_VCPKG_TAG} not found upstream; falling back to local source tree.")
        set(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../..")
    else()
        # git ls-remote may return one or two lines (annotated tag adds a ^{} deref line). Take the last line and extract the commit SHA.
        string(REPLACE "\n" ";" AERONET_TAG_LINES "${AERONET_TAG_QUERY}")
        list(GET AERONET_TAG_LINES -1 AERONET_TAG_LAST)
        string(REGEX MATCH "^[0-9a-fA-F]+" AERONET_TAG_COMMIT "${AERONET_TAG_LAST}")
        if(NOT AERONET_TAG_COMMIT)
            message(WARNING "[aeronet] Could not parse commit for tag ${AERONET_VCPKG_TAG}; falling back to local source tree.")
            set(SOURCE_PATH "${CMAKE_CURRENT_LIST_DIR}/../..")
        else()
            message(STATUS "[aeronet] Using commit ${AERONET_TAG_COMMIT} for tag ${AERONET_VCPKG_TAG}")
            vcpkg_from_git(
                OUT_SOURCE_PATH SOURCE_PATH
                URL https://github.com/sjanel/aeronet.git
                REF ${AERONET_TAG_COMMIT}
            )
        endif()
    endif()
endif()

set(AERONET_ENABLE_OPENSSL OFF)
if("tls" IN_LIST FEATURES)
    set(AERONET_ENABLE_OPENSSL ON)
endif()

set(AERONET_ENABLE_SPDLOG OFF)
if("spdlog" IN_LIST FEATURES)
    set(AERONET_ENABLE_SPDLOG ON)
endif()

# Map vcpkg linkage to project shared/static toggle
if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    set(AERONET_BUILD_SHARED ON)
else()
    set(AERONET_BUILD_SHARED OFF)
endif()

message(STATUS "[aeronet] Config: OPENSSL=${AERONET_ENABLE_OPENSSL} SPDLOG=${AERONET_ENABLE_SPDLOG} SHARED=${AERONET_BUILD_SHARED}")

vcpkg_cmake_configure(
    SOURCE_PATH ${SOURCE_PATH}
    OPTIONS
        -DAERONET_BUILD_TESTS=OFF
        -DAERONET_BUILD_EXAMPLES=OFF
        -DAERONET_INSTALL=ON
        -DAERONET_ENABLE_OPENSSL=${AERONET_ENABLE_OPENSSL}
        -DAERONET_ENABLE_SPDLOG=${AERONET_ENABLE_SPDLOG}
        -DCMAKE_CXX_STANDARD=23
        -DAERONET_BUILD_SHARED=${AERONET_BUILD_SHARED}
)

vcpkg_cmake_install()

# Copy license
file(INSTALL ${SOURCE_PATH}/LICENSE DESTINATION ${CURRENT_PACKAGES_DIR}/share/aeronet RENAME copyright)

vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/aeronet)

# Remove debug include duplication if any
file(REMOVE_RECURSE ${CURRENT_PACKAGES_DIR}/debug/include)
