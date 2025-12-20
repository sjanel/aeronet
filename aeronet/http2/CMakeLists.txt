# HTTP/2 module (RFC 9113)

aux_source_directory(src/ AERONET_HTTP2_SRC_LIST)

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
)

AeronetAddUnitTest(
    http2-frame_test
    test/http2-frame_test.cpp
    LIBRARIES
    aeronet_http2
)

AeronetAddUnitTest(
    http2-protocol-handler_test
    test/http2-protocol-handler_test.cpp
    LIBRARIES
    aeronet_http2
)

AeronetAddUnitTest(
    http2-stream_test
    test/http2-stream_test.cpp
    LIBRARIES
    aeronet_http2
)