aux_source_directory(src AERONET_TLS_SRC_LIST)

AeronetAddProjectLibrary(aeronet_tls
    ${AERONET_TLS_SRC_LIST}
)

# Public includes
target_include_directories(aeronet_tls
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

find_package(OpenSSL REQUIRED)

# Link against OpenSSL::SSL and OpenSSL::Crypto (imported by find_package) and base tech lib for utils.
target_link_libraries(aeronet_tls
    PRIVATE
        OpenSSL::SSL
        OpenSSL::Crypto
    PUBLIC
        aeronet_sys
        aeronet_objects
        aeronet_tech
)

# TLS unit tests use socketpair(AF_UNIX) and/or dlsym/RTLD_NEXT overrides — Linux only.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")

AeronetAddUnitTest(
    tls-components_test
    test/tls-components_test.cpp
    LIBRARIES
    aeronet_tls
    aeronet_test_support
)

AeronetAddUnitTest(
    tls-handshake_test
    test/tls-handshake_test.cpp
    LIBRARIES
    aeronet_tls
    aeronet_test_support
)

AeronetAddUnitTest(
    tls-ticket-key-store_test
    src/tls-ticket-key-store.cpp
    test/tls-ticket-key-store_test.cpp
    LIBRARIES
    aeronet_objects
    OpenSSL::SSL
    OpenSSL::Crypto
)

if(AERONET_BUILD_TESTS)
    target_link_libraries(tls-ticket-key-store_test PRIVATE dl)
endif()

endif()