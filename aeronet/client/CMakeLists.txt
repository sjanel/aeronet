aux_source_directory(src AERONET_CLIENT_SRC_LIST)

AeronetAddProjectLibrary(aeronet_client
    ${AERONET_CLIENT_SRC_LIST}
)

# Reuse existing aeronet bricks: http (HttpResponse container, http types), sys (event loop,
# transports, TCP connector, socket ops), objects (http version, flat_hash_map).
target_link_libraries(aeronet_client
    PUBLIC aeronet_http aeronet_sys aeronet_objects
)

target_include_directories(aeronet_client
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/aeronet/http/include>
        $<INSTALL_INTERFACE:include>
)

if(AERONET_ENABLE_OPENSSL)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(aeronet_client PRIVATE OpenSSL::SSL OpenSSL::Crypto aeronet_tls)
    target_include_directories(aeronet_client PRIVATE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/aeronet/tls/include>)
endif()

# --- Tests ---

AeronetAddUnitTest(
    url_test
    test/url_test.cpp
    LIBRARIES
    aeronet_client
)

AeronetAddUnitTest(
    response-parser_test
    test/response-parser_test.cpp
    LIBRARIES
    aeronet_client
)
if(TARGET response-parser_test)
    target_include_directories(response-parser_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

# Parser-level automatic decompression coverage (needs the compression test helpers).
AeronetAddUnitTest(
    response-parser-decompress_test
    test/response-parser-decompress_test.cpp
    LIBRARIES
    aeronet_client
    aeronet_test_support
)
if(TARGET response-parser-decompress_test)
    target_include_directories(response-parser-decompress_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
endif()

# Pure-config / pure-builder unit tests (no sockets).
AeronetAddUnitTest(
    http-client-config_test
    test/http-client-config_test.cpp
    LIBRARIES
    aeronet_client
)

AeronetAddUnitTest(
    retry-config_test
    test/retry-config_test.cpp
    LIBRARIES
    aeronet_client
)

AeronetAddUnitTest(
    client-request_test
    test/client-request_test.cpp
    LIBRARIES
    aeronet_client
)

AeronetAddUnitTest(
    client-protocol_test
    test/client-protocol_test.cpp
    LIBRARIES
    aeronet_client
)

AeronetAddUnitTest(
    http-client-exception_test
    test/http-client-exception_test.cpp
    LIBRARIES
    aeronet_client
)

AeronetAddUnitTest(
    http-client-error_test
    test/http-client-error_test.cpp
    LIBRARIES
    aeronet_client
)

# End-to-end tests drive the client against a live aeronet server.
AeronetAddUnitTest(
    http-client-e2e_test
    test/http-client-e2e_test.cpp
    LIBRARIES
    aeronet_client
    aeronet
)

# Error / retry paths driven by a hand-rolled raw TCP server (abrupt close, dead pool, timeout, DNS).
AeronetAddUnitTest(
    http-client-error-e2e_test
    test/http-client-error-e2e_test.cpp
    LIBRARIES
    aeronet_client
)

# End-to-end automatic compression / decompression against a live aeronet server.
AeronetAddUnitTest(
    http-client-compression-e2e_test
    test/http-client-compression-e2e_test.cpp
    LIBRARIES
    aeronet_client
    aeronet
    aeronet_test_support
)

if(AERONET_ENABLE_OPENSSL)
    AeronetAddUnitTest(
        http-client-tls-e2e_test
        test/http-client-tls-e2e_test.cpp
        LIBRARIES
        aeronet_client
        aeronet
        aeronet_basic_test_support
    )
endif()
