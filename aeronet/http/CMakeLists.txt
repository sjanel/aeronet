aux_source_directory(src AERONET_HTTP_SRC_LIST)

# Remove upgrade-handler.cpp if neither HTTP/2 nor WebSocket is enabled
if(NOT AERONET_ENABLE_HTTP2 AND NOT AERONET_ENABLE_WEBSOCKET)
    list(REMOVE_ITEM AERONET_HTTP_SRC_LIST src/upgrade-handler.cpp)
endif()

AeronetAddProjectLibrary(aeronet_http
    ${AERONET_HTTP_SRC_LIST}
)

target_link_libraries(aeronet_http
    PUBLIC aeronet_tech aeronet_objects aeronet_sys
)

if(AERONET_ENABLE_OPENSSL)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(aeronet_http PRIVATE OpenSSL::Crypto)
    target_link_libraries(aeronet_http PRIVATE aeronet_tls)
endif()

if(AERONET_ENABLE_WEBSOCKET)
    target_link_libraries(aeronet_http PUBLIC aeronet_websocket)
endif()

target_include_directories(aeronet_http
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<$<BOOL:${AERONET_ENABLE_OPENSSL}>:$<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/aeronet/tls/include>>
        $<INSTALL_INTERFACE:include>
)

AeronetAddUnitTest(
    connection-state_test
    test/connection-state_test.cpp
    LIBRARIES
    aeronet_http
    aeronet_test_support
)

AeronetAddUnitTest(
    cors-policy_test
    test/cors-policy_test.cpp
    LIBRARIES
    aeronet_http
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    header-line-parse_test
    test/header-line-parse_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    http-codec_test
    test/http-codec_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    http-error-build_test
    test/http-error-build_test.cpp
    LIBRARIES
    aeronet_http
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    http-headers-view_test
    test/http-headers-view_test.cpp
    LIBRARIES
    aeronet_http
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    http-method_test
    test/http-method_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    http-partial-head-write_test
    test/http-partial-head-write_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    http-response_test
    test/http-response_test.cpp
    LIBRARIES
    aeronet_http aeronet_sys aeronet_test_support
)

AeronetAddUnitTest(
    http-request_test
    test/http-request_test.cpp
    LIBRARIES
    aeronet_http
    aeronet_basic_test_support
)

AeronetAddUnitTest(
    mergeable-headers_test
    test/mergeable-headers_test.cpp
    LIBRARIES
    aeronet_tech
)

AeronetAddUnitTest(
    multipart-form-data_test
    test/multipart-form-data_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    path-handler-entry_test
    test/path-handler-entry_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    router_test
    test/router_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    static-file-handler_test
    test/static-file-handler_test.cpp
    LIBRARIES
    aeronet_http
    aeronet_test_support
)

if (AERONET_ENABLE_HTTP2 OR AERONET_ENABLE_WEBSOCKET)
    AeronetAddUnitTest(
        upgrade-handler_test
        test/upgrade-handler_test.cpp
        LIBRARIES
        aeronet_http
        aeronet_basic_test_support
    )
endif()