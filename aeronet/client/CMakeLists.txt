aux_source_directory(src AERONET_CLIENT_SRC_LIST)

if(NOT AERONET_ENABLE_HTTP2)
    list(REMOVE_ITEM AERONET_CLIENT_SRC_LIST src/http2-connection.cpp)
endif()

if(NOT AERONET_ENABLE_OPENSSL)
    list(REMOVE_ITEM AERONET_CLIENT_SRC_LIST src/http-client-tls-context.cpp)
endif()

AeronetAddProjectLibrary(aeronet_client
    ${AERONET_CLIENT_SRC_LIST}
)

# Reuse existing aeronet bricks: http (HttpResponse container, http types), sys (event loop,
# transports, TCP connector, socket ops), objects (http version, flat_hash_map).
target_link_libraries(aeronet_client
    PUBLIC aeronet_http aeronet_sys aeronet_objects
)

# Native HTTP/2 client engine: reuses the server's HPACK + frame codecs (Http2Connection).
if(AERONET_ENABLE_HTTP2)
    target_link_libraries(aeronet_client PRIVATE aeronet_http2)
    target_include_directories(aeronet_client PRIVATE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/aeronet/http2/include>)
endif()

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
    client-accept-encoding_test
    test/client-accept-encoding_test.cpp
    LIBRARIES
    aeronet_objects
)

AeronetAddUnitTest(
    client-protocol_test
    test/client-protocol_test.cpp
    LIBRARIES
    aeronet_client
)

AeronetAddUnitTest(
    http-client-config_test
    test/http-client-config_test.cpp
    LIBRARIES
    aeronet_objects
    aeronet_client
)

AeronetAddUnitTest(
    http-client-core_test
    test/http-client-core_test.cpp
    LIBRARIES
    aeronet_client
    aeronet
    aeronet_test_support
)

AeronetAddUnitTest(
    http-client-exception_test
    test/http-client-exception_test.cpp
    LIBRARIES
    aeronet_client
)

# Native HTTP/2 client engine: h2c prior knowledge over plain http, Auto/Http1_1/Http2 version modes,
# flow control both ways, pooling; plus ALPN negotiation over TLS when OpenSSL is enabled.
if(AERONET_ENABLE_HTTP2)
    AeronetAddUnitTest(
        http-client-http2-e2e_test
        test/http-client-http2-e2e_test.cpp
        LIBRARIES
        aeronet_client
        aeronet
        aeronet_test_support
    )
endif()

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

AeronetAddUnitTest(
    http-request_test
    test/http-request_test.cpp
    LIBRARIES
    aeronet_client
    aeronet_test_support
)

AeronetAddUnitTest(
    response-parser_test
    test/response-parser_test.cpp
    LIBRARIES
    aeronet_client
    aeronet_test_support
)

AeronetAddUnitTest(
    retry-config_test
    test/retry-config_test.cpp
    LIBRARIES
    aeronet_client
)