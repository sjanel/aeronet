aux_source_directory(src/ AERONET_HTTP_SRC_LIST)

add_project_library(aeronet_http
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

add_unit_test(
    connection_state_sendfile_test
    test/connection_state_sendfile_test.cpp
    LIBRARIES
    aeronet_http aeronet_test_support
)

add_unit_test(
    http-error-build_test
    test/http-error-build_test.cpp
    LIBRARIES
    aeronet_http
)

add_unit_test(
    http-response_test
    test/http-response_test.cpp
    LIBRARIES
    aeronet_http aeronet_sys aeronet_test_support
)

add_unit_test(
    http-request_test
    test/http-request_test.cpp
    LIBRARIES
    aeronet_http
)

add_unit_test(
    mergeable-headers_test
    test/mergeable-headers_test.cpp
    LIBRARIES
    aeronet_tech
)

add_unit_test(
    static-file-handler_test
    test/static-file-handler_test.cpp
    LIBRARIES
    aeronet_http
    aeronet_test_support
)