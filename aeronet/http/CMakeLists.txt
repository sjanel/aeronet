aux_source_directory(src/ AERONET_HTTP_SRC_LIST)

AeronetAddProjectLibrary(aeronet_http
    ${AERONET_HTTP_SRC_LIST}
)

target_link_libraries(aeronet_http
    PUBLIC aeronet_tech aeronet_objects aeronet_sys
)

if(AERONET_ENABLE_OPENSSL)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(aeronet_http PRIVATE OpenSSL::Crypto)
endif()

target_include_directories(aeronet_http
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
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
)

AeronetAddUnitTest(
    header-line-parse_test
    test/header-line-parse_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    http-error-build_test
    test/http-error-build_test.cpp
    LIBRARIES
    aeronet_http
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
    http-response-fuzz_test
    test/http-response-fuzz_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    http-request_test
    test/http-request_test.cpp
    LIBRARIES
    aeronet_http
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

AeronetAddUnitTest(
    upgrade-handler_test
    test/upgrade-handler_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    websocket-deflate_test
    test/websocket-deflate_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    websocket-endpoint_test
    test/websocket-endpoint_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    websocket-frame_test
    test/websocket-frame_test.cpp
    LIBRARIES
    aeronet_http
)

AeronetAddUnitTest(
    websocket-handler_test
    test/websocket-handler_test.cpp
    LIBRARIES
    aeronet_http
)