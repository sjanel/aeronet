# Header-only: version.hpp reports the versions of the dependencies (OpenSSL, glaze, zlib, zstd, brotli, spdlog),
# whose headers it includes. It sits above aeronet_objects so that only the libraries exposing it (client, server)
# need the OpenSSL headers, not every module.
add_library(aeronet_version INTERFACE)

target_include_directories(aeronet_version
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(aeronet_version INTERFACE aeronet_objects)

if(AERONET_ENABLE_OPENSSL)
    find_package(OpenSSL REQUIRED)
    # OPENSSL_VERSION_TEXT is read from the OpenSSL headers.
    target_link_libraries(aeronet_version INTERFACE OpenSSL::Crypto)
endif()

AeronetAddUnitTest(
    version_test
    test/version_test.cpp
    LIBRARIES
    aeronet_version
)
