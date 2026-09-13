# HTTP/2 module (RFC 9113)

aux_source_directory(src/ AERONET_HTTP2_SRC_LIST)

if(NOT AERONET_ENABLE_HTTP_SERVER)
    list(REMOVE_ITEM AERONET_HTTP2_SRC_LIST src/http2-protocol-handler.cpp)
endif()

AeronetAddProjectLibrary(aeronet_http2
    ${AERONET_HTTP2_SRC_LIST}
)

target_link_libraries(aeronet_http2
    PUBLIC aeronet_tech aeronet_objects aeronet_http
)

target_include_directories(aeronet_http2
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# Unit tests
AeronetAddUnitTest(
    hpack_test
    test/hpack_test.cpp
    LIBRARIES
    aeronet_http2
)

AeronetAddUnitTest(
    http2-config_test
    test/http2-config_test.cpp
    LIBRARIES
    aeronet_http2
)

AeronetAddUnitTest(
    http2-connection_test
    test/http2-connection_test.cpp
    LIBRARIES
    aeronet_http2
    aeronet_basic_test_support
    aeronet_test_support
)

AeronetAddUnitTest(
    http2-frame_test
    test/http2-frame_test.cpp
    LIBRARIES
    aeronet_http2
)

if(AERONET_ENABLE_HTTP_SERVER)
    AeronetAddUnitTest(
        http2-protocol-handler_test
        test/http2-protocol-handler_test.cpp
        LIBRARIES
        aeronet_http2
        aeronet_basic_test_support
        aeronet_test_support
    )
endif()

AeronetAddUnitTest(
    http2-stream_test
    test/http2-stream_test.cpp
    LIBRARIES
    aeronet_http2
)