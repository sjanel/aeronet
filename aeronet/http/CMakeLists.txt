aux_source_directory(src/ AERONET_HTTP_SRC_LIST)

AeronetAddProjectLibrary(aeronet_http
    ${AERONET_HTTP_SRC_LIST}
)

target_link_libraries(aeronet_http
    PUBLIC aeronet_tech aeronet_objects aeronet_sys
)

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
    static-file-handler_test
    test/static-file-handler_test.cpp
    LIBRARIES
    aeronet_http
    aeronet_test_support
)